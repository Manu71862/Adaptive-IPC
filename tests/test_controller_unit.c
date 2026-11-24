/**
 * test_controller_unit.c — Comprehensive Unit Tests for Adaptive Controller,
 * Credit Accounting, ACK Tracking, and Backpressure Invariants.
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Exhaustively verifies:
 *  1. Normal DATA -> ACK -> credit replenishment lifecycle.
 *  2. Duplicate ACK rejection with zero counter decrement / phantom capacity.
 *  3. Unknown or already-completed ACK rejection.
 *  4. Sender exit with unused credits: immediate reclamation from outstanding_work.
 *  5. Sender disconnect with in-flight work: graceful drainage without credit leaks.
 *  6. Budget reduction below outstanding work: hard withholding for new work.
 *  7. Recovery after backlog drains below reduced budget.
 *  8. NORMAL, CAUTION, and OVERLOAD three-state hysteresis automaton.
 *  9. Counter underflow protection on completion and credit reclamation.
 * 10. Rate-imbalance-driven congestion transitions (rho > 1.10 and rho > 1.30).
 * 11. Multi-step hysteresis recovery, hold-count reset, sustained overload budget floor, and threshold boundaries.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controller.h"
#include "common.h"

static int g_passed = 0;
static int g_total = 0;

#define TEST_CHECK(cond, msg) do { \
    g_total++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_passed++; \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
    } \
} while (0)

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║     UNIT TESTS: ADAPTIVE CONTROLLER & CREDIT ACCOUNTING          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    /* ── 1. Initialization ──────────────────────────────────────────── */
    printf("[1/11] Testing Controller Initialization & Invariants...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);
        TEST_CHECK(ctrl.current_state == STATE_NORMAL, "Initial state is STATE_NORMAL");
        TEST_CHECK(ctrl.current_budget == 64, "Initial budget is 64 credits");
        TEST_CHECK(ctrl.min_budget == 8 && ctrl.max_budget == 128, "Budget bounds are [8 .. 128]");
        TEST_CHECK(ctrl.outstanding_work == 0, "Initial outstanding work is 0");
    }

    /* ── 2. NORMAL, CAUTION, and OVERLOAD Controller States ──────────── */
    printf("\n[2/11] Testing NORMAL, CAUTION, and OVERLOAD Hysteresis Dynamics...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);

        /* NORMAL: Moderate arrivals and completions -> state remains NORMAL */
        for (int i = 0; i < 20; i++) controller_on_message_arrival(&ctrl, 0, 512);
        for (int i = 0; i < 20; i++) controller_on_message_completion(&ctrl, 0);
        controller_tick(&ctrl, 0.20, 2);
        TEST_CHECK(ctrl.current_state == STATE_NORMAL, "State remains NORMAL at 20% queue utilization");

        /* CAUTION: Queue utilization reaches 50% with balanced rates -> escalates to CAUTION */
        for (int i = 0; i < 50; i++) controller_on_message_arrival(&ctrl, 0, 512);
        for (int i = 0; i < 50; i++) controller_on_message_completion(&ctrl, 0);
        controller_tick(&ctrl, 0.55, 2);
        TEST_CHECK(ctrl.current_state == STATE_CAUTION, "State transitions to CAUTION at 55% utilization");

        /* OVERLOAD: Severe burst and queue utilization >= 75% -> escalates to OVERLOAD */
        for (int i = 0; i < 100; i++) controller_on_message_arrival(&ctrl, 0, 512);
        controller_tick(&ctrl, 0.80, 2);
        TEST_CHECK(ctrl.current_state == STATE_OVERLOAD, "State transitions to OVERLOAD at 80% utilization");
        TEST_CHECK(ctrl.current_budget < 64, "Budget multiplicatively throttled under OVERLOAD");

        /* Hysteresis hold: First low utilization interval holds OVERLOAD */
        controller_tick(&ctrl, 0.15, 2);
        TEST_CHECK(ctrl.current_state == STATE_OVERLOAD, "Hysteresis holds OVERLOAD on first low-utilization interval");

        /* Second low interval: recovers to NORMAL */
        controller_tick(&ctrl, 0.15, 2);
        TEST_CHECK(ctrl.current_state == STATE_NORMAL, "Hysteresis recovers to NORMAL on second consecutive interval");
    }

    /* ── 3. Normal DATA -> ACK -> Replenishment Lifecycle ─────────────── */
    printf("\n[3/11] Testing Normal DATA -> ACK -> Replenishment Lifecycle...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);
        ctrl.current_budget = 32;

        SenderCreditTracker tr;
        tracker_init(&tr);

        /* Grant 16 initial credits to sender 0 */
        uint32_t grant = controller_grant_credits_for_sender(&ctrl, 0, 2, tracker_total_outstanding(&tr));
        TEST_CHECK(grant == 16, "Initial credit grant admits 16 credits (32 / 2)");
        tracker_on_credit_granted(&tr, grant);
        TEST_CHECK(ctrl.outstanding_work == 16, "Outstanding work increments to 16");
        TEST_CHECK(tr.credits_held == 16 && tr.messages_in_flight == 0, "Tracker records 16 held credits");

        /* Sender transmits DATA message with seq = 101 */
        bool sent = tracker_on_data_sent(&tr, 101);
        TEST_CHECK(sent, "DATA message seq 101 registered in tracker");
        TEST_CHECK(tr.credits_held == 15, "Credits held decrements to 15");
        TEST_CHECK(tr.messages_in_flight == 1, "Messages in flight increments to 1");
        TEST_CHECK(tracker_total_outstanding(&tr) == 16, "Total sender allocation remains constant (16)");

        /* Receiver processes message and returns valid ACK for seq = 101 */
        bool ack_ok = tracker_on_ack_received(&tr, 101);
        TEST_CHECK(ack_ok, "Valid ACK for seq 101 accepted by tracker");
        TEST_CHECK(tr.messages_in_flight == 0, "In-flight messages decrements to 0");
        TEST_CHECK(tracker_total_outstanding(&tr) == 15, "Total sender allocation drops to 15");

        controller_on_message_completion(&ctrl, 0);
        TEST_CHECK(ctrl.outstanding_work == 15, "Outstanding work decrements to 15 after completion");

        /* Prompt replenishment: 1 credit should be granted */
        uint32_t replen = controller_grant_credits_for_sender(&ctrl, 0, 2, tracker_total_outstanding(&tr));
        TEST_CHECK(replen == 1, "Replenishment grants exactly 1 credit to restore target (16)");
        tracker_on_credit_granted(&tr, replen);
        TEST_CHECK(ctrl.outstanding_work == 16, "Outstanding work restored to 16");
        TEST_CHECK(tr.credits_held == 16, "Held credits restored to 16");
    }

    /* ── 4. Duplicate ACK Rejection ──────────────────────────────────── */
    printf("\n[4/11] Testing Duplicate ACK Rejection...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);
        ctrl.outstanding_work = 10;

        SenderCreditTracker tr;
        tracker_init(&tr);
        tracker_on_credit_granted(&tr, 5);
        tracker_on_data_sent(&tr, 201); /* seq 201 in flight */

        /* First ACK for seq 201: valid */
        bool first_ack = tracker_on_ack_received(&tr, 201);
        TEST_CHECK(first_ack, "First ACK for seq 201 accepted");

        /* Second (duplicate) ACK for seq 201 */
        bool dup_ack = tracker_on_ack_received(&tr, 201);
        TEST_CHECK(!dup_ack, "Duplicate ACK for seq 201 rejected");
        TEST_CHECK(tr.messages_in_flight == 0, "In-flight count not decremented on duplicate ACK");

        /* Third duplicate ACK */
        bool dup_ack2 = tracker_on_ack_received(&tr, 201);
        TEST_CHECK(!dup_ack2, "Repeated duplicate ACK rejected");
    }

    /* ── 5. ACK for Unknown or Already-Completed Message ─────────────── */
    printf("\n[5/11] Testing ACK for Unknown or Already-Completed Message...\n");
    {
        SenderCreditTracker tr;
        tracker_init(&tr);
        tracker_on_credit_granted(&tr, 5);
        tracker_on_data_sent(&tr, 301);

        /* ACK for unrecorded sequence number 999 */
        bool bad_ack = tracker_on_ack_received(&tr, 999);
        TEST_CHECK(!bad_ack, "ACK for unrecorded sequence 999 rejected");
        TEST_CHECK(tr.messages_in_flight == 1, "In-flight count unchanged on unknown ACK");

        /* ACK for sequence 0 */
        bool zero_ack = tracker_on_ack_received(&tr, 0);
        TEST_CHECK(!zero_ack, "ACK for sequence 0 rejected");
    }

    /* ── 6. Sender Exit with Unused Credits (Immediate Reclamation) ──── */
    printf("\n[6/11] Testing Sender Exit with Unused Credits (Reclamation)...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);
        ctrl.current_budget = 32;

        SenderCreditTracker tr;
        tracker_init(&tr);
        uint32_t grant = controller_grant_credits_for_sender(&ctrl, 0, 2, 0);
        tracker_on_credit_granted(&tr, grant); /* 16 credits granted */
        TEST_CHECK(ctrl.outstanding_work == 16, "Controller records 16 outstanding credits");

        /* Sender transmits 6 messages (seqs 1..6), leaving 10 unused credits */
        for (uint64_t s = 1; s <= 6; s++) {
            tracker_on_data_sent(&tr, s);
        }
        TEST_CHECK(tr.credits_held == 10, "Sender holds 10 unspent credits");
        TEST_CHECK(tr.messages_in_flight == 6, "Sender has 6 in-flight messages");

        /* Sender now exits / sends MSG_STOP: reclaim unused credits */
        uint32_t unused = tracker_reclaim_unused(&tr);
        TEST_CHECK(unused == 10, "tracker_reclaim_unused returns exactly 10 unspent credits");
        TEST_CHECK(tr.credits_held == 0, "Held credits reset to 0 after reclamation");

        controller_reclaim_credits(&ctrl, unused);
        TEST_CHECK(ctrl.outstanding_work == 6, "Outstanding work decrements from 16 to 6 upon sender exit");
        TEST_CHECK(ctrl.outstanding_work == tr.messages_in_flight,
                   "Remaining outstanding work matches exact in-flight message count");
    }

    /* ── 7. Sender Disconnect with In-Flight Work (Drainage) ──────────── */
    printf("\n[7/11] Testing Sender Disconnect with In-Flight Work Drainage...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);

        SenderCreditTracker tr;
        tracker_init(&tr);
        tracker_on_credit_granted(&tr, 4);
        ctrl.outstanding_work = 4;

        /* All 4 credits sent as DATA messages */
        for (uint64_t s = 10; s < 14; s++) {
            tracker_on_data_sent(&tr, s);
        }
        TEST_CHECK(tr.credits_held == 0 && tr.messages_in_flight == 4, "4 messages in-flight");

        /* Sender disconnects (credits_held is 0, nothing unused to reclaim) */
        uint32_t unused = tracker_reclaim_unused(&tr);
        TEST_CHECK(unused == 0, "0 unused credits to reclaim");

        /* In-flight messages are acknowledged by receiver one by one */
        for (uint64_t s = 10; s < 14; s++) {
            bool ok = tracker_on_ack_received(&tr, s);
            TEST_CHECK(ok, "In-flight message ACKed after sender disconnect");
            controller_on_message_completion(&ctrl, 0);
        }

        TEST_CHECK(tr.messages_in_flight == 0, "All in-flight messages drained to 0");
        TEST_CHECK(ctrl.outstanding_work == 0, "Outstanding work cleanly reached 0 without leak");
    }

    /* ── 8. Budget Reduction Below Outstanding Work & Backlog Drainage ─ */
    printf("\n[8/11] Testing Budget Reduction Below Outstanding Work & Recovery...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);
        ctrl.current_budget = 32;
        ctrl.outstanding_work = 30;

        /* Controller reduces budget to 8 (e.g. OVERLOAD under receiver stall) */
        ctrl.current_budget = 8;

        /* outstanding_work (30) > current_budget (8): new credit grants MUST be withheld */
        uint32_t grant = controller_grant_credits_for_sender(&ctrl, 0, 2, 0);
        TEST_CHECK(grant == 0, "No new credits admitted when outstanding_work (30) >= current_budget (8)");

        /* Backlog drains through valid ACKs: 26 messages acknowledged */
        for (int i = 0; i < 26; i++) {
            controller_on_message_completion(&ctrl, 0);
        }
        TEST_CHECK(ctrl.outstanding_work == 4, "Outstanding work drained down to 4");

        /* Now outstanding_work (4) < current_budget (8): credits resume admission! */
        grant = controller_grant_credits_for_sender(&ctrl, 0, 2, 0);
        TEST_CHECK(grant > 0, "Credit admission safely resumes once outstanding work drains below budget");
        TEST_CHECK(grant <= 4, "Admitted credits bounded by per-sender limit (8 / 2 = 4)");
        TEST_CHECK(ctrl.outstanding_work <= ctrl.current_budget, "Outstanding work remains bounded by current_budget");
    }

    /* ── 9. Counter Underflow Protection ─────────────────────────────── */
    printf("\n[9/11] Testing Counter Underflow Protection...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);
        ctrl.outstanding_work = 0;

        /* Extraneous completion when outstanding_work is already 0 */
        controller_on_message_completion(&ctrl, 0);
        TEST_CHECK(ctrl.outstanding_work == 0, "outstanding_work does not underflow on extra completion");

        /* Excessive reclamation */
        controller_reclaim_credits(&ctrl, 100);
        TEST_CHECK(ctrl.outstanding_work == 0, "outstanding_work does not underflow on excessive reclamation");

        /* Tracker underflow protection */
        SenderCreditTracker tr;
        tracker_init(&tr);
        bool ack_empty = tracker_on_ack_received(&tr, 1);
        TEST_CHECK(!ack_empty, "tracker rejects ACK when in-flight count is 0");
        TEST_CHECK(tr.messages_in_flight == 0, "messages_in_flight does not underflow");

        uint32_t reclaim_empty = tracker_reclaim_unused(&tr);
        TEST_CHECK(reclaim_empty == 0, "reclaim_unused returns 0 on empty tracker");
        TEST_CHECK(tr.credits_held == 0, "credits_held does not underflow");
    }

    /* ── 10. Rate-Imbalance-Driven Congestion Transitions ────────────── */
    printf("\n[10/11] Testing Rate-Imbalance-Driven Congestion Transitions...\n");
    {
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);
        TEST_CHECK(ctrl.current_state == STATE_NORMAL, "Initial state is NORMAL");
        TEST_CHECK(ctrl.state_transitions == 0, "Initial state transitions is 0");

        /* Rate-driven transition: NORMAL -> CAUTION (util=0.35 >= 0.30, rho > 1.10) */
        for (int i = 0; i < 50; i++) controller_on_message_arrival(&ctrl, 0, 512);
        for (int i = 0; i < 10; i++) controller_on_message_completion(&ctrl, 0);
        controller_tick(&ctrl, 0.35, 2);

        double rho1 = ctrl.arrival_rate_ewma / (ctrl.completion_rate_ewma + 1.0);
        TEST_CHECK(rho1 > 1.10, "Arrival-to-service rate ratio rho > 1.10");
        TEST_CHECK(ctrl.current_state == STATE_CAUTION, "Escalates to CAUTION on moderate util (0.35) when rho > 1.10");
        TEST_CHECK(ctrl.state_transitions == 1, "state_transitions counter increments to 1");

        /* Rate-driven transition: CAUTION -> OVERLOAD (util=0.55 >= 0.50, rho > 1.30) */
        /* Severe completion starvation: 80 arrivals, 0 completions */
        for (int i = 0; i < 80; i++) controller_on_message_arrival(&ctrl, 0, 512);
        controller_tick(&ctrl, 0.55, 2);

        double rho2 = ctrl.arrival_rate_ewma / (ctrl.completion_rate_ewma + 1.0);
        TEST_CHECK(rho2 > 1.30, "Arrival-to-service rate ratio rho > 1.30 under completion stall");
        TEST_CHECK(ctrl.current_state == STATE_OVERLOAD, "Escalates to OVERLOAD on util (0.55) when rho > 1.30");
        TEST_CHECK(ctrl.state_transitions == 2, "state_transitions counter increments to 2");
        TEST_CHECK(ctrl.current_budget < 64, "Budget throttled under rate-induced OVERLOAD");
    }

    /* ── 11. Multi-Step Recovery, Hysteresis Reset, Boundary & Floor ─── */
    printf("\n[11/11] Testing Step-Down Recovery, Hysteresis Reset, Boundary & Floor...\n");
    {
        /* A. Step-Down Recovery: OVERLOAD -> CAUTION -> NORMAL with 2-tick holds */
        AdaptiveController ctrl;
        controller_init(&ctrl, CONFIG_C_ADAPTIVE_FIFO);

        /* Escalate directly to OVERLOAD at 80% util */
        controller_tick(&ctrl, 0.80, 2);
        TEST_CHECK(ctrl.current_state == STATE_OVERLOAD, "Escalates to OVERLOAD at 80% util");
        TEST_CHECK(ctrl.state_transitions == 1, "Transitions counter is 1");

        /* Step down towards CAUTION (util=0.55, balanced rates):
         * Tick 1: hold_count = 1, remains OVERLOAD */
        controller_tick(&ctrl, 0.55, 2);
        TEST_CHECK(ctrl.current_state == STATE_OVERLOAD, "Hold interval 1: holds OVERLOAD state");
        TEST_CHECK(ctrl.hysteresis_hold_count == 1, "Hysteresis hold count increments to 1");

        /* Tick 2: hold_count reaches 2 -> transitions to CAUTION */
        controller_tick(&ctrl, 0.55, 2);
        TEST_CHECK(ctrl.current_state == STATE_CAUTION, "Hold interval 2: transitions to CAUTION");
        TEST_CHECK(ctrl.hysteresis_hold_count == 0, "Hold count resets to 0 after transition");
        TEST_CHECK(ctrl.state_transitions == 2, "Transitions counter increments to 2");

        /* Step down towards NORMAL (util=0.20, balanced rates):
         * Tick 1: hold_count = 1, remains CAUTION */
        controller_tick(&ctrl, 0.20, 2);
        TEST_CHECK(ctrl.current_state == STATE_CAUTION, "Hold interval 1: holds CAUTION state");
        TEST_CHECK(ctrl.hysteresis_hold_count == 1, "Hysteresis hold count increments to 1");

        /* Tick 2: hold_count reaches 2 -> transitions to NORMAL */
        controller_tick(&ctrl, 0.20, 2);
        TEST_CHECK(ctrl.current_state == STATE_NORMAL, "Hold interval 2: transitions to NORMAL");
        TEST_CHECK(ctrl.hysteresis_hold_count == 0, "Hold count resets to 0 after transition");
        TEST_CHECK(ctrl.state_transitions == 3, "Transitions counter increments to 3");

        /* B. Hysteresis Hold Reset on Transient Re-Escalation */
        AdaptiveController ctrl2;
        controller_init(&ctrl2, CONFIG_C_ADAPTIVE_FIFO);
        controller_tick(&ctrl2, 0.80, 2);
        TEST_CHECK(ctrl2.current_state == STATE_OVERLOAD, "ctrl2 escalates to OVERLOAD");

        /* Tick 1 at low util: hold_count = 1 */
        controller_tick(&ctrl2, 0.15, 2);
        TEST_CHECK(ctrl2.hysteresis_hold_count == 1, "Hold count is 1 after first calm interval");

        /* Tick 2: Sudden congestion re-spike to 0.80 -> hold_count MUST reset to 0 */
        controller_tick(&ctrl2, 0.80, 2);
        TEST_CHECK(ctrl2.current_state == STATE_OVERLOAD, "ctrl2 remains in OVERLOAD on re-spike");
        TEST_CHECK(ctrl2.hysteresis_hold_count == 0, "Hold count resets to 0 upon re-spike to OVERLOAD");

        /* C. Sustained Overload Multiplicative Decrease Floor */
        AdaptiveController ctrl3;
        controller_init(&ctrl3, CONFIG_C_ADAPTIVE_FIFO);
        bool all_bounded = true;
        for (int tick = 0; tick < 10; tick++) {
            controller_tick(&ctrl3, 0.85, 2);
            if (ctrl3.current_budget < ctrl3.min_budget) all_bounded = false;
        }
        TEST_CHECK(all_bounded, "Budget stays >= min_budget during sustained overload");
        TEST_CHECK(ctrl3.current_budget == ctrl3.min_budget, "Budget clamps at exactly min_budget (8)");
        TEST_CHECK(ctrl3.current_budget == 8, "Minimum budget verified as 8 credits");

        /* D. Exact Boundary Threshold Verification */
        AdaptiveController ctrl4;
        controller_init(&ctrl4, CONFIG_C_ADAPTIVE_FIFO);

        /* 0.499 utilization: strictly below 0.50 threshold */
        controller_tick(&ctrl4, 0.499, 2);
        TEST_CHECK(ctrl4.current_state == STATE_NORMAL, "0.499 utilization remains in STATE_NORMAL");

        /* 0.500 utilization: meets 0.50 threshold */
        controller_tick(&ctrl4, 0.500, 2);
        TEST_CHECK(ctrl4.current_state == STATE_CAUTION, "0.500 utilization transitions to STATE_CAUTION");

        /* 0.749 utilization: strictly below 0.75 threshold */
        controller_tick(&ctrl4, 0.749, 2);
        TEST_CHECK(ctrl4.current_state == STATE_CAUTION, "0.749 utilization remains in STATE_CAUTION");

        /* 0.750 utilization: meets 0.75 threshold */
        controller_tick(&ctrl4, 0.750, 2);
        TEST_CHECK(ctrl4.current_state == STATE_OVERLOAD, "0.750 utilization transitions to STATE_OVERLOAD");
    }

    printf("\n════════════════════════════════════════════════════════════════════\n");
    printf("Controller & Credit Accounting Unit Tests: %d / %d Passed.\n", g_passed, g_total);
    printf("════════════════════════════════════════════════════════════════════\n\n");
    return (g_passed == g_total) ? 0 : 1;
}
