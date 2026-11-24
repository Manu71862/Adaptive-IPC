/**
 * controller.h — Adaptive Backpressure Controller & EWMA Rate Estimator
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Implements:
 *  - EWMA rate estimation for ingress arrivals and receiver completions
 *  - 3-State hysteresis machine: NORMAL, CAUTION, OVERLOAD
 *  - Bounded global admission budgeting with non-revocable credit withholding
 *  - Gradual recovery dynamics preventing burst collapse post-stall
 *  - Support for Configurations A (Fixed-Window), B (Fixed-Threshold), C & D (Adaptive)
 */
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "ipc_protocol.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONFIG_A_FIXED_WINDOW    = 0, /* Fixed credit window baseline, FIFO dispatch */
    CONFIG_B_FIXED_THRESHOLD = 1, /* Fixed high/low queue threshold backpressure, FIFO dispatch */
    CONFIG_C_ADAPTIVE_FIFO   = 2, /* Adaptive EWMA backpressure, FIFO dispatch */
    CONFIG_D_ADAPTIVE_DRR    = 3  /* Adaptive EWMA backpressure, Deficit Round-Robin dispatch */
} ExperimentConfigType;

typedef enum {
    STATE_NORMAL   = 0,
    STATE_CAUTION  = 1,
    STATE_OVERLOAD = 2
} ControllerState;

typedef struct {
    ExperimentConfigType config_type;
    ControllerState      current_state;

    /* EWMA Rate Estimators (messages per second) */
    double ewma_alpha;           /* Smoothing weight (default: 0.20) */
    double arrival_rate_ewma;    /* R_in */
    double completion_rate_ewma; /* R_comp */

    /* Interval Counters */
    uint64_t last_tick_ns;
    uint32_t interval_arrivals;
    uint32_t interval_completions;

    /* Admission Budget Bounds */
    uint32_t min_budget;         /* e.g., 8 credits */
    uint32_t max_budget;         /* e.g., 128 credits */
    uint32_t current_budget;     /* Dynamically adjusted admission budget */
    uint32_t fixed_window_per_sender; /* For Config A (default: 16) */

    /* Outstanding Work Accounting */
    uint32_t total_granted_credits;
    uint32_t total_acked_messages;
    uint32_t outstanding_work;   /* granted - acked */

    /* Hysteresis & Recovery Counters */
    int hysteresis_hold_count;   /* Consecutive intervals required to lower state */
    int recovery_step;           /* Gradual budget recovery increment */
    uint32_t state_transitions;  /* Total state changes logged */

    /* Controller Execution Profiling */
    double total_overhead_us;
    uint64_t tick_count;
} AdaptiveController;

/* ── Sender Credit & In-Flight Work Tracker ──────────────────────── */
#define MAX_INFLIGHT_SEQS 128

typedef struct {
    uint32_t credits_held;        /* Credits granted but not yet consumed by MSG_DATA */
    uint32_t messages_in_flight;  /* MSG_DATA messages received, awaiting valid ACK */
    uint64_t inflight_seqs[MAX_INFLIGHT_SEQS];
    size_t   inflight_count;
} SenderCreditTracker;

void tracker_init(SenderCreditTracker *tr);
void tracker_on_credit_granted(SenderCreditTracker *tr, uint32_t grant);
bool tracker_on_data_sent(SenderCreditTracker *tr, uint64_t seq);
bool tracker_on_ack_received(SenderCreditTracker *tr, uint64_t seq);
uint32_t tracker_reclaim_unused(SenderCreditTracker *tr);
uint32_t tracker_total_outstanding(const SenderCreditTracker *tr);

/* ── Controller API ──────────────────────────────────────────────── */

void controller_init(AdaptiveController *c, ExperimentConfigType type);

/** Records a new incoming message arrival from a sender */
void controller_on_message_arrival(AdaptiveController *c, int sender_id, size_t payload_len);

/** Records a completion acknowledgement from the receiver */
void controller_on_message_completion(AdaptiveController *c, int sender_id);

/** Safely reclaims unused credits (e.g. on sender exit) from outstanding_work */
void controller_reclaim_credits(AdaptiveController *c, uint32_t credits);

/**
 * Invoked every CONTROLLER_INTERVAL_MS (50 ms) by the broker event loop.
 * Updates EWMA rates, assesses queue utilization, executes hysteresis state
 * machine, and computes the new admission budget.
 *
 * @param current_queue_utilization Ratio [0.0 .. 1.0] of broker queue depth
 * @param active_senders Number of currently connected senders
 */
void controller_tick(AdaptiveController *c, double current_queue_utilization, int active_senders);

/**
 * Computes how many new credits can be safely admitted for a specific sender.
 * Enforces global budget withholding (never revokes existing outstanding work)
 * and per-sender allocation bounds.
 */
uint32_t controller_grant_credits_for_sender(AdaptiveController *c, int sender_id, int active_senders, uint32_t sender_outstanding);

const char *controller_state_str(ControllerState st);
const char *controller_config_str(ExperimentConfigType cfg);

#ifdef __cplusplus
}
#endif

#endif /* CONTROLLER_H */
