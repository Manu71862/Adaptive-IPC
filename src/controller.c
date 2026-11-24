/**
 * controller.c — Implementation of Adaptive Backpressure & Rate Control
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 */
#include "controller.h"
#include "common.h"
#include <math.h>

void controller_init(AdaptiveController *c, ExperimentConfigType type) {
    if (!c) return;
    memset(c, 0, sizeof(AdaptiveController));

    c->config_type = type;
    c->current_state = STATE_NORMAL;
    c->ewma_alpha = 0.20;

    c->min_budget = 8;
    c->max_budget = 128;
    c->current_budget = 64;
    c->fixed_window_per_sender = 16;
    c->recovery_step = 4;

    c->last_tick_ns = get_time_ns();
}

void controller_on_message_arrival(AdaptiveController *c, int sender_id, size_t payload_len) {
    (void)sender_id;
    (void)payload_len;
    if (!c) return;
    c->interval_arrivals++;
}

void controller_on_message_completion(AdaptiveController *c, int sender_id) {
    (void)sender_id;
    if (!c) return;
    c->interval_completions++;
    c->total_acked_messages++;
    if (c->outstanding_work > 0) {
        c->outstanding_work--;
    }
}

void controller_reclaim_credits(AdaptiveController *c, uint32_t credits) {
    if (!c || credits == 0) return;
    if (c->outstanding_work >= credits) {
        c->outstanding_work -= credits;
    } else {
        c->outstanding_work = 0;
    }
}

/* ── Sender Credit & In-Flight Work Tracker Implementation ────────── */

void tracker_init(SenderCreditTracker *tr) {
    if (!tr) return;
    memset(tr, 0, sizeof(SenderCreditTracker));
}

void tracker_on_credit_granted(SenderCreditTracker *tr, uint32_t grant) {
    if (!tr) return;
    tr->credits_held += grant;
}

bool tracker_on_data_sent(SenderCreditTracker *tr, uint64_t seq) {
    if (!tr) return false;
    if (tr->credits_held > 0) {
        tr->credits_held--;
    }
    if (tr->inflight_count < MAX_INFLIGHT_SEQS) {
        tr->inflight_seqs[tr->inflight_count++] = seq;
        tr->messages_in_flight++;
        return true;
    }
    return false;
}

bool tracker_on_ack_received(SenderCreditTracker *tr, uint64_t seq) {
    if (!tr || tr->inflight_count == 0) return false;
    for (size_t k = 0; k < tr->inflight_count; k++) {
        if (tr->inflight_seqs[k] == seq) {
            /* Found active in-flight sequence; remove it */
            tr->inflight_seqs[k] = tr->inflight_seqs[tr->inflight_count - 1];
            tr->inflight_count--;
            if (tr->messages_in_flight > 0) {
                tr->messages_in_flight--;
            }
            return true;
        }
    }
    return false; /* Unknown, duplicate, or stale sequence number */
}

uint32_t tracker_reclaim_unused(SenderCreditTracker *tr) {
    if (!tr) return 0;
    uint32_t unused = tr->credits_held;
    tr->credits_held = 0;
    return unused;
}

uint32_t tracker_total_outstanding(const SenderCreditTracker *tr) {
    if (!tr) return 0;
    return tr->credits_held + tr->messages_in_flight;
}

void controller_tick(AdaptiveController *c, double current_queue_utilization, int active_senders) {
    if (!c) return;

    uint64_t t0 = get_time_ns();
    double dt = (double)(t0 - c->last_tick_ns) * 1e-9;
    if (dt <= 0.001) dt = 0.050; /* Fallback if called too rapidly */
    c->last_tick_ns = t0;

    /* Instantaneous interval rates */
    double measured_arr = (double)c->interval_arrivals / dt;
    double measured_comp = (double)c->interval_completions / dt;
    c->interval_arrivals = 0;
    c->interval_completions = 0;

    /* Update EWMA rates */
    if (c->tick_count == 0) {
        c->arrival_rate_ewma = measured_arr;
        c->completion_rate_ewma = measured_comp;
    } else {
        c->arrival_rate_ewma = (c->ewma_alpha * measured_arr) + ((1.0 - c->ewma_alpha) * c->arrival_rate_ewma);
        c->completion_rate_ewma = (c->ewma_alpha * measured_comp) + ((1.0 - c->ewma_alpha) * c->completion_rate_ewma);
    }

    /* ── Execute Configuration-Specific Policy ───────────────────── */
    if (c->config_type == CONFIG_A_FIXED_WINDOW) {
        /* Config A: Static Window — no dynamic adaptation */
        c->current_state = STATE_NORMAL;
        c->current_budget = (active_senders > 0) ? (active_senders * c->fixed_window_per_sender) : 32;
    }
    else if (c->config_type == CONFIG_B_FIXED_THRESHOLD) {
        /* Config B: Fixed Threshold Backpressure */
        if (current_queue_utilization >= 0.75) {
            c->current_state = STATE_OVERLOAD;
            c->current_budget = c->min_budget;
        } else if (current_queue_utilization <= 0.25) {
            c->current_state = STATE_NORMAL;
            c->current_budget = c->max_budget;
        }
    }
    else {
        /* Config C & D: Full Adaptive EWMA + Hysteresis + Gradual Recovery */
        double rho = c->arrival_rate_ewma / (c->completion_rate_ewma + 1.0);

        ControllerState desired_state = STATE_NORMAL;
        if (current_queue_utilization >= 0.75 || (current_queue_utilization >= 0.50 && rho > 1.30)) {
            desired_state = STATE_OVERLOAD;
        } else if (current_queue_utilization >= 0.50 || (current_queue_utilization >= 0.30 && rho > 1.10)) {
            desired_state = STATE_CAUTION;
        } else {
            desired_state = STATE_NORMAL;
        }

        /* Hysteresis: immediate escalation; delayed de-escalation */
        if (desired_state > c->current_state) {
            c->current_state = desired_state;
            c->hysteresis_hold_count = 0;
            c->state_transitions++;
        } else if (desired_state < c->current_state) {
            c->hysteresis_hold_count++;
            if (c->hysteresis_hold_count >= 2) { /* Hold 2 intervals (~100ms) before stepping down */
                c->current_state = desired_state;
                c->hysteresis_hold_count = 0;
                c->state_transitions++;
            }
        } else {
            c->hysteresis_hold_count = 0;
        }

        /* Budget Modulation Based on State */
        switch (c->current_state) {
            case STATE_OVERLOAD: {
                /* Multiplicative Decrease to throttle admission immediately */
                uint32_t dec = (uint32_t)(c->current_budget * 0.65);
                c->current_budget = (dec > c->min_budget) ? dec : c->min_budget;
                c->recovery_step = 2;
                break;
            }
            case STATE_CAUTION: {
                /* Stabilize budget around receiver completion capacity (100ms target horizon) */
                double target_comp = c->completion_rate_ewma * 0.100;
                uint32_t target_b = (uint32_t)target_comp;
                if (target_b < c->min_budget) target_b = c->min_budget;
                if (target_b > c->max_budget * 0.60) target_b = (uint32_t)(c->max_budget * 0.60);
                c->current_budget = (c->current_budget + target_b) / 2;
                c->recovery_step = 4;
                break;
            }
            case STATE_NORMAL: {
                /* Gradual Recovery: additive increase prevents burst collapse */
                if (c->current_budget + c->recovery_step <= c->max_budget) {
                    c->current_budget += c->recovery_step;
                } else {
                    c->current_budget = c->max_budget;
                }
                if (c->recovery_step < 16) c->recovery_step += 2;
                break;
            }
        }
    }

    uint64_t t1 = get_time_ns();
    c->total_overhead_us += (double)(t1 - t0) * 1e-3;
    c->tick_count++;
}

uint32_t controller_grant_credits_for_sender(AdaptiveController *c, int sender_id, int active_senders, uint32_t sender_outstanding) {
    if (!c || sender_id < 0 || sender_id >= MAX_SENDERS) return 0;
    if (active_senders <= 0) active_senders = 1;

    if (c->config_type == CONFIG_A_FIXED_WINDOW) {
        uint32_t target = c->fixed_window_per_sender;
        if (sender_outstanding < target) {
            uint32_t grant = target - sender_outstanding;
            c->total_granted_credits += grant;
            c->outstanding_work += grant;
            return grant;
        }
        return 0;
    }

    /*
     * For Configs B, C, D — bounded global budget withholding.
     *
     * Enforces current_budget as the hard admission ceiling for new work.
     * Accounts for already-issued non-revocable credits when the budget decreases:
     * If outstanding work (unconsumed credits held by senders + in-flight messages)
     * reaches or exceeds current_budget, new credits are withheld until outstanding
     * work drains below the new target budget via ACKs or credit reclamation.
     */
    if (c->outstanding_work >= c->current_budget) {
        return 0;
    }

    uint32_t available_global = c->current_budget - c->outstanding_work;
    uint32_t max_per_sender = c->current_budget / (uint32_t)active_senders;
    if (max_per_sender < 1) max_per_sender = 1;

    if (sender_outstanding < max_per_sender) {
        uint32_t requested = max_per_sender - sender_outstanding;
        uint32_t grant = (requested < available_global) ? requested : available_global;

        c->total_granted_credits += grant;
        c->outstanding_work += grant;
        return grant;
    }

    return 0;
}

const char *controller_state_str(ControllerState st) {
    switch (st) {
        case STATE_NORMAL:   return "NORMAL";
        case STATE_CAUTION:  return "CAUTION";
        case STATE_OVERLOAD: return "OVERLOAD";
        default:             return "UNKNOWN";
    }
}

const char *controller_config_str(ExperimentConfigType cfg) {
    switch (cfg) {
        case CONFIG_A_FIXED_WINDOW:    return "Config A (Fixed Window, FIFO)";
        case CONFIG_B_FIXED_THRESHOLD: return "Config B (Fixed Threshold, FIFO)";
        case CONFIG_C_ADAPTIVE_FIFO:   return "Config C (Adaptive EWMA, FIFO)";
        case CONFIG_D_ADAPTIVE_DRR:    return "Config D (Adaptive EWMA, DRR)";
        default:                       return "UNKNOWN";
    }
}
