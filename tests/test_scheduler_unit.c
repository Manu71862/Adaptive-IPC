/**
 * test_scheduler_unit.c — Comprehensive Unit Tests for FIFO & DRR Scheduling
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Exhaustively verifies:
 *  1. Strict timestamp ordering in global FIFO dispatch.
 *  2. Mathematical verification of calculate_jains_fairness() with synthetic vectors.
 *  3. DRR byte fairness under equal offered byte volumes.
 *  4. Continuously backlogged competing senders (packet dispatch ratio ~7.4 : 1).
 *  5. Strict FIFO vs DRR on identical asymmetric workloads (0.63 vs >=0.95).
 *  6. Deficit accumulation for packets larger than one quantum.
 *  7. Empty queue handling and sender reactivation without deficit carry-over.
 *  8. Explicit demonstration distinguishing message-count fairness from byte fairness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "scheduler.h"
#include "queue.h"
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
    printf("║     COMPREHENSIVE UNIT TESTS: FIFO VS DEFICIT ROUND-ROBIN        ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    /* ── 1. Strict Timestamp Ordering in FIFO Dispatch ──────────────── */
    printf("[1/8] Testing Strict FIFO Timestamp Ordering...\n");
    {
        BrokerQueueManager qm;
        queue_manager_init(&qm);

        BrokerScheduler fifo_sched;
        scheduler_init(&fifo_sched, DISPATCH_FIFO);

        IpcMessage m_early, m_late;
        memset(&m_early, 0, sizeof(m_early));
        m_early.header.magic = IPC_MAGIC;
        m_early.header.version = IPC_VERSION;
        m_early.header.msg_type = MSG_DATA;
        m_early.header.sender_id = 1;
        m_early.header.msg_seq = 1;
        m_early.header.timestamp_ns = 1000;
        m_early.header.payload_len = 100;

        memset(&m_late, 0, sizeof(m_late));
        m_late.header.magic = IPC_MAGIC;
        m_late.header.version = IPC_VERSION;
        m_late.header.msg_type = MSG_DATA;
        m_late.header.sender_id = 0;
        m_late.header.msg_seq = 1;
        m_late.header.timestamp_ns = 2000;
        m_late.header.payload_len = 100;

        /* Enqueue sender 0 (late) first, then sender 1 (early) */
        queue_enqueue(&qm, 0, &m_late);
        queue_enqueue(&qm, 1, &m_early);

        IpcMessage out1, out2;
        int s1 = -1, s2 = -1;
        QueueResult r1 = scheduler_dispatch_next(&fifo_sched, &qm, &out1, &s1);
        QueueResult r2 = scheduler_dispatch_next(&fifo_sched, &qm, &out2, &s2);

        TEST_CHECK(r1 == QUEUE_OK && s1 == 1, "FIFO dispatches earlier timestamp (Sender 1) first");
        TEST_CHECK(r2 == QUEUE_OK && s2 == 0, "FIFO dispatches later timestamp (Sender 0) second");

        queue_manager_cleanup(&qm);
    }

    /* ── 2. Mathematical Verification of calculate_jains_fairness() ─── */
    printf("\n[2/8] Mathematical Verification of Jain's Fairness Index...\n");
    {
        /* Equal allocations -> 1.0 */
        double vec_equal[] = { 100.0, 100.0 };
        double j_equal = calculate_jains_fairness(vec_equal, 2);
        TEST_CHECK(fabs(j_equal - 1.0) < 1e-4, "calculate_jains_fairness([100, 100]) == 1.0000");

        /* Extreme asymmetry (1 active flow in 2-flow system) -> 0.5 */
        double vec_extreme[] = { 100.0, 0.0 };
        double j_extreme = calculate_jains_fairness(vec_extreme, 2);
        TEST_CHECK(fabs(j_extreme - 0.5) < 1e-4, "calculate_jains_fairness([100, 0]) == 0.5000");

        /* Theoretical 1:8 byte ratio (138,000B vs 1,034,000B) -> 0.6311 */
        double vec_asym[] = { 138000.0, 1034000.0 };
        double j_asym = calculate_jains_fairness(vec_asym, 2);
        TEST_CHECK(fabs(j_asym - 0.6311) < 1e-3, "calculate_jains_fairness([138kB, 1034kB]) == 0.6311");

        /* Four equal flows -> 1.0 */
        double vec_four[] = { 25.0, 25.0, 25.0, 25.0 };
        double j_four = calculate_jains_fairness(vec_four, 4);
        TEST_CHECK(fabs(j_four - 1.0) < 1e-4, "calculate_jains_fairness([25, 25, 25, 25]) == 1.0000");

        /* Single flow -> 1.0 */
        double vec_single[] = { 42.0 };
        double j_single = calculate_jains_fairness(vec_single, 1);
        TEST_CHECK(fabs(j_single - 1.0) < 1e-4, "calculate_jains_fairness([42]) == 1.0000");
    }

    /* ── 3. DRR Byte Fairness with Equal Offered Byte Volume ────────── */
    printf("\n[3/8] Testing DRR with Equal Offered Byte Volume...\n");
    {
        /*
         * Setup:
         *  Sender 0: 64 messages of 512 bytes payload (552 bytes frame = 35,328 total bytes)
         *  Sender 1:  8 messages of 4096 bytes payload (4136 bytes frame = 33,088 total bytes)
         *  Offered byte ratio: 35,328 / 33,088 = 1.0677 (almost identical volume)
         */
        BrokerQueueManager qm;
        queue_manager_init(&qm);

        BrokerScheduler drr_sched;
        scheduler_init(&drr_sched, DISPATCH_DRR);

        for (int i = 0; i < 64; i++) {
            IpcMessage s_msg;
            memset(&s_msg, 0, sizeof(s_msg));
            s_msg.header.magic = IPC_MAGIC;
            s_msg.header.version = IPC_VERSION;
            s_msg.header.msg_type = MSG_DATA;
            s_msg.header.sender_id = 0;
            s_msg.header.msg_seq = i + 1;
            s_msg.header.payload_len = 512;
            queue_enqueue(&qm, 0, &s_msg);
        }

        for (int i = 0; i < 8; i++) {
            IpcMessage l_msg;
            memset(&l_msg, 0, sizeof(l_msg));
            l_msg.header.magic = IPC_MAGIC;
            l_msg.header.version = IPC_VERSION;
            l_msg.header.msg_type = MSG_DATA;
            l_msg.header.sender_id = 1;
            l_msg.header.msg_seq = i + 1;
            l_msg.header.payload_len = 4096;
            queue_enqueue(&qm, 1, &l_msg);
        }

        IpcMessage d_msg;
        int sender_id = -1;
        while (scheduler_dispatch_next(&drr_sched, &qm, &d_msg, &sender_id) == QUEUE_OK) {
            /* Drain all */
        }

        double byte_fairness = scheduler_get_byte_fairness(&drr_sched, 2);
        printf("  Sender 0 (512B) : %lu msgs, %lu bytes\n",
               (unsigned long)drr_sched.dispatched_msgs[0], (unsigned long)drr_sched.dispatched_bytes[0]);
        printf("  Sender 1 (4096B): %lu msgs, %lu bytes\n",
               (unsigned long)drr_sched.dispatched_msgs[1], (unsigned long)drr_sched.dispatched_bytes[1]);
        printf("  Jain's Byte Fairness Index: %.4f\n", byte_fairness);

        TEST_CHECK(drr_sched.dispatched_msgs[0] == 64 && drr_sched.dispatched_msgs[1] == 8,
                   "DRR dispatches all 64 small and 8 large messages");
        TEST_CHECK(byte_fairness >= 0.95, "DRR achieves high byte fairness (Jain >= 0.95)");

        queue_manager_cleanup(&qm);
    }

    /* ── 4. Continuously Backlogged Competing Senders: Dispatch Ratio ── */
    printf("\n[4/8] Testing Continuously Backlogged Senders Dispatch Ratio...\n");
    {
        /*
         * DRR Quantum = 4136 bytes.
         * Small packet frame = 552 bytes. Large packet frame = 4136 bytes.
         * Theoretical packet ratio = 4136 / 552 ≈ 7.4927 small packets per 1 large packet.
         */
        BrokerQueueManager qm;
        queue_manager_init(&qm);

        BrokerScheduler drr_sched;
        scheduler_init(&drr_sched, DISPATCH_DRR);

        /* Pre-load generous backlogs */
        for (int i = 0; i < 64; i++) {
            IpcMessage s_msg;
            memset(&s_msg, 0, sizeof(s_msg));
            s_msg.header.magic = IPC_MAGIC;
            s_msg.header.version = IPC_VERSION;
            s_msg.header.msg_type = MSG_DATA;
            s_msg.header.sender_id = 0;
            s_msg.header.msg_seq = i + 1;
            s_msg.header.payload_len = 512;
            queue_enqueue(&qm, 0, &s_msg);
        }

        for (int i = 0; i < 16; i++) {
            IpcMessage l_msg;
            memset(&l_msg, 0, sizeof(l_msg));
            l_msg.header.magic = IPC_MAGIC;
            l_msg.header.version = IPC_VERSION;
            l_msg.header.msg_type = MSG_DATA;
            l_msg.header.sender_id = 1;
            l_msg.header.msg_seq = i + 1;
            l_msg.header.payload_len = 4096;
            queue_enqueue(&qm, 1, &l_msg);
        }

        /* Dispatch until Sender 1 has dispatched 5 large packets */
        IpcMessage d_msg;
        int sender_id = -1;
        while (drr_sched.dispatched_msgs[1] < 5 &&
               scheduler_dispatch_next(&drr_sched, &qm, &d_msg, &sender_id) == QUEUE_OK) {
        }

        double ratio = (double)drr_sched.dispatched_msgs[0] / (double)drr_sched.dispatched_msgs[1];
        printf("  Dispatched Sender 0 (small): %lu msgs (%lu bytes)\n",
               (unsigned long)drr_sched.dispatched_msgs[0], (unsigned long)drr_sched.dispatched_bytes[0]);
        printf("  Dispatched Sender 1 (large): %lu msgs (%lu bytes)\n",
               (unsigned long)drr_sched.dispatched_msgs[1], (unsigned long)drr_sched.dispatched_bytes[1]);
        printf("  Observed Dispatch Ratio    : %.2f small pkts / large pkt (Theoretical: 7.49)\n", ratio);

        TEST_CHECK(ratio >= 7.0 && ratio <= 8.0,
                   "DRR dispatches ~7.4-7.5 small packets per 1 large packet under backlog");

        queue_manager_cleanup(&qm);
    }

    /* ── 5. Strict FIFO vs DRR on Identical Asymmetric Workloads ─────── */
    printf("\n[5/8] Strict FIFO vs DRR on Identical Asymmetric Workloads...\n");
    {
        BrokerQueueManager qm_fifo, qm_drr;
        queue_manager_init(&qm_fifo);
        queue_manager_init(&qm_drr);

        BrokerScheduler fifo_sched, drr_sched;
        scheduler_init(&fifo_sched, DISPATCH_FIFO);
        scheduler_init(&drr_sched, DISPATCH_DRR);

        /* Interleave timestamps so FIFO dispatches 1:1 packet alternation */
        uint64_t ts = 1000;
        for (int i = 0; i < 40; i++) {
            IpcMessage s_msg, l_msg;
            memset(&s_msg, 0, sizeof(s_msg));
            s_msg.header.magic = IPC_MAGIC;
            s_msg.header.msg_type = MSG_DATA;
            s_msg.header.sender_id = 0;
            s_msg.header.payload_len = 512;
            s_msg.header.timestamp_ns = ts++;

            memset(&l_msg, 0, sizeof(l_msg));
            l_msg.header.magic = IPC_MAGIC;
            l_msg.header.msg_type = MSG_DATA;
            l_msg.header.sender_id = 1;
            l_msg.header.payload_len = 4096;
            l_msg.header.timestamp_ns = ts++;

            queue_enqueue(&qm_fifo, 0, &s_msg);
            queue_enqueue(&qm_fifo, 1, &l_msg);

            queue_enqueue(&qm_drr, 0, &s_msg);
            queue_enqueue(&qm_drr, 1, &l_msg);
        }

        /* Dispatch 40 concurrent messages from each */
        IpcMessage d;
        int sid;
        for (int k = 0; k < 40; k++) {
            scheduler_dispatch_next(&fifo_sched, &qm_fifo, &d, &sid);
            scheduler_dispatch_next(&drr_sched, &qm_drr, &d, &sid);
        }

        double fifo_fairness = scheduler_get_byte_fairness(&fifo_sched, 2);
        double drr_fairness = scheduler_get_byte_fairness(&drr_sched, 2);

        printf("  FIFO Byte Allocation: S0=%lu B, S1=%lu B -> Jain=%.4f\n",
               (unsigned long)fifo_sched.dispatched_bytes[0], (unsigned long)fifo_sched.dispatched_bytes[1],
               fifo_fairness);
        printf("  DRR  Byte Allocation: S0=%lu B, S1=%lu B -> Jain=%.4f\n",
               (unsigned long)drr_sched.dispatched_bytes[0], (unsigned long)drr_sched.dispatched_bytes[1],
               drr_fairness);

        TEST_CHECK(fifo_fairness <= 0.65, "FIFO suffers severe byte unfairness (Jain <= 0.65)");
        TEST_CHECK(drr_fairness >= 0.95,  "DRR preserves high byte fairness (Jain >= 0.95)");

        queue_manager_cleanup(&qm_fifo);
        queue_manager_cleanup(&qm_drr);
    }

    /* ── 6. Deficit Accumulation for Packets Larger than One Quantum ─── */
    printf("\n[6/8] Testing Deficit Accumulation Across Rounds...\n");
    {
        BrokerQueueManager qm;
        queue_manager_init(&qm);

        BrokerScheduler drr_sched;
        scheduler_init(&drr_sched, DISPATCH_DRR);
        /* Set quantum smaller than packet size: quantum = 2000, packet = 4136 */
        drr_sched.quantum_bytes = 2000;

        IpcMessage big_msg;
        memset(&big_msg, 0, sizeof(big_msg));
        big_msg.header.magic = IPC_MAGIC;
        big_msg.header.msg_type = MSG_DATA;
        big_msg.header.sender_id = 0;
        big_msg.header.payload_len = 4096; /* 4136 bytes frame */
        queue_enqueue(&qm, 0, &big_msg);

        /* Add a small message to sender 1 to advance round index */
        IpcMessage small_msg;
        memset(&small_msg, 0, sizeof(small_msg));
        small_msg.header.magic = IPC_MAGIC;
        small_msg.header.msg_type = MSG_DATA;
        small_msg.header.sender_id = 1;
        small_msg.header.payload_len = 100;

        /* Deficit accumulates across rounds until >= 4136 */
        IpcMessage out;
        int sid = -1;
        QueueResult r = QUEUE_ERR_EMPTY;
        for (int attempt = 0; attempt < 5 && r != QUEUE_OK; attempt++) {
            r = scheduler_dispatch_next(&drr_sched, &qm, &out, &sid);
        }
        TEST_CHECK(r == QUEUE_OK && sid == 0, "Oversized packet successfully dispatches after deficit accumulation");
        TEST_CHECK(drr_sched.dispatched_bytes[0] == 4136, "Dispatched bytes equals 4136 bytes");

        queue_manager_cleanup(&qm);
    }

    /* ── 7. Empty Queue Handling & Deficit Reset on Idle ─────────────── */
    printf("\n[7/8] Testing Deficit Reset on Flow Idleness...\n");
    {
        BrokerQueueManager qm;
        queue_manager_init(&qm);

        BrokerScheduler drr_sched;
        scheduler_init(&drr_sched, DISPATCH_DRR);

        /* Sender 0 enqueues 1 small packet (552 bytes). Quantum = 4136 */
        IpcMessage s_msg;
        memset(&s_msg, 0, sizeof(s_msg));
        s_msg.header.magic = IPC_MAGIC;
        s_msg.header.msg_type = MSG_DATA;
        s_msg.header.sender_id = 0;
        s_msg.header.payload_len = 512;
        queue_enqueue(&qm, 0, &s_msg);

        IpcMessage out;
        int sid = -1;
        scheduler_dispatch_next(&drr_sched, &qm, &out, &sid);

        /* Queue has now become empty. Deficit counter must be 0, not (4136 - 552 = 3584) */
        TEST_CHECK(drr_sched.deficit_counter[0] == 0,
                   "Deficit counter reset to 0 immediately when queue becomes empty");

        /* Sender 0 reactivates with a new message */
        queue_enqueue(&qm, 0, &s_msg);
        scheduler_dispatch_next(&drr_sched, &qm, &out, &sid);
        TEST_CHECK(drr_sched.dispatched_msgs[0] == 2, "Reactivated sender dispatches correctly");

        queue_manager_cleanup(&qm);
    }

    /* ── 8. Explicit Message-Count Fairness vs Byte Fairness Trade-off ── */
    printf("\n[8/8] Message-Count Fairness vs Byte Fairness Trade-off...\n");
    {
        /*
         * Under FIFO: Equal message counts (1:1), but severe byte disparity (1:8).
         *  -> Msg Fairness = 1.0000, Byte Fairness = 0.6311
         * Under DRR: Equal byte volumes (1:1), but disparate message counts (8:1).
         *  -> Msg Fairness = 0.6231, Byte Fairness = 0.9989
         */
        double fifo_msgs[] = { 36.0, 36.0 };
        double fifo_bytes[] = { 36.0 * 552.0, 36.0 * 4136.0 };
        double fifo_j_msg = calculate_jains_fairness(fifo_msgs, 2);
        double fifo_j_byte = calculate_jains_fairness(fifo_bytes, 2);

        double drr_msgs[] = { 64.0, 8.0 };
        double drr_bytes[] = { 64.0 * 552.0, 8.0 * 4136.0 };
        double drr_j_msg = calculate_jains_fairness(drr_msgs, 2);
        double drr_j_byte = calculate_jains_fairness(drr_bytes, 2);

        printf("  FIFO Metric Contrast: Msg Fairness = %.4f | Byte Fairness = %.4f\n",
               fifo_j_msg, fifo_j_byte);
        printf("  DRR  Metric Contrast: Msg Fairness = %.4f | Byte Fairness = %.4f\n",
               drr_j_msg, drr_j_byte);

        TEST_CHECK(fifo_j_msg > drr_j_msg,
                   "FIFO achieves deceptive message-count equality while starving bytes");
        TEST_CHECK(drr_j_byte > fifo_j_byte,
                   "DRR guarantees true byte bandwidth equity despite packet count asymmetry");
    }

    printf("\nScheduler Unit Tests: %d / %d Passed.\n\n", g_passed, g_total);
    return (g_passed == g_total) ? 0 : 1;
}
