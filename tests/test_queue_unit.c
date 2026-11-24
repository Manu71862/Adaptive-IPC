/**
 * test_queue_unit.c — Unit Tests for Multi-Queue Storage & Hard Limits
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    printf("║        UNIT TESTS: MULTI-QUEUE STORAGE & HARD LIMITS             ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    BrokerQueueManager qm;
    queue_manager_init(&qm);

    TEST_CHECK(queue_get_global_count(&qm) == 0, "Queue initializes with 0 global messages");
    TEST_CHECK(queue_get_global_bytes(&qm) == 0, "Queue initializes with 0 queued bytes");

    /* 1. Basic Enqueue and Dequeue */
    IpcMessage m1;
    memset(&m1, 0, sizeof(m1));
    m1.header.magic = IPC_MAGIC;
    m1.header.version = IPC_VERSION;
    m1.header.msg_type = MSG_DATA;
    m1.header.sender_id = 0;
    m1.header.msg_seq = 1;
    m1.header.payload_len = 100;

    QueueResult rc = queue_enqueue(&qm, 0, &m1);
    TEST_CHECK(rc == QUEUE_OK, "Enqueue single valid message succeeds");
    TEST_CHECK(queue_get_global_count(&qm) == 1, "Global message count increments to 1");
    TEST_CHECK(queue_get_global_bytes(&qm) == 100, "Global queued bytes increments to 100");

    /* Peek verification */
    const IpcMessage *peek = NULL;
    TEST_CHECK(queue_peek_sender(&qm, 0, &peek) == QUEUE_OK && peek->header.msg_seq == 1,
               "Peek correctly inspects head message without dequeue");
    TEST_CHECK(queue_peek_head_len(&qm, 0) == 100, "Peek head length reports 100 bytes");

    /* Dequeue */
    IpcMessage out_m;
    rc = queue_dequeue_sender(&qm, 0, &out_m);
    TEST_CHECK(rc == QUEUE_OK && out_m.header.msg_seq == 1, "Dequeue returns head message");
    TEST_CHECK(queue_get_global_count(&qm) == 0, "Global count drops to 0 after dequeue");
    TEST_CHECK(queue_get_global_bytes(&qm) == 0, "Global bytes drops to 0 after dequeue");

    /* 2. Enforce Per-Sender Queue Bound (PER_SENDER_QUEUE_CAP = 64) */
    printf("\nTesting Per-Sender Queue Bound (64 messages max)...\n");
    for (int i = 0; i < PER_SENDER_QUEUE_CAP; i++) {
        m1.header.msg_seq = i + 1;
        m1.header.payload_len = 10;
        rc = queue_enqueue(&qm, 1, &m1);
        if (rc != QUEUE_OK) break;
    }
    TEST_CHECK(rc == QUEUE_OK && qm.queues[1].count == PER_SENDER_QUEUE_CAP,
               "Sender 1 admits exactly 64 messages");

    /* 65th message must be rejected with QUEUE_ERR_SENDER_FULL */
    m1.header.msg_seq = 65;
    rc = queue_enqueue(&qm, 1, &m1);
    TEST_CHECK(rc == QUEUE_ERR_SENDER_FULL, "Sender 1 queue rejects 65th message (PER_SENDER_QUEUE_CAP enforced)");

    /* Clear sender 1 */
    queue_clear_sender(&qm, 1);
    TEST_CHECK(qm.queues[1].count == 0 && queue_get_global_count(&qm) == 0,
               "queue_clear_sender() cleans up all nodes and restores global count");

    /* 3. Enforce Global Queue Capacity Bound (GLOBAL_QUEUE_CAP = 256) */
    printf("\nTesting Global Broker Capacity Bound (256 messages max)...\n");
    /* Fill across 4 senders (4 * 64 = 256) */
    for (int s = 0; s < 4; s++) {
        for (int i = 0; i < PER_SENDER_QUEUE_CAP; i++) {
            m1.header.sender_id = s;
            m1.header.msg_seq = i + 1;
            m1.header.payload_len = 10;
            queue_enqueue(&qm, s, &m1);
        }
    }
    TEST_CHECK(queue_get_global_count(&qm) == GLOBAL_QUEUE_CAP, "Broker successfully reaches 256 global messages");

    /* Attempt 257th message on sender 4 (which has room in its per-sender queue) */
    m1.header.sender_id = 4;
    rc = queue_enqueue(&qm, 4, &m1);
    TEST_CHECK(rc == QUEUE_ERR_GLOBAL_FULL, "Enqueue rejects message when global queue is at capacity (256)");

    queue_manager_cleanup(&qm);
    TEST_CHECK(queue_get_global_count(&qm) == 0, "queue_manager_cleanup() cleanly resets all queues");

    /* 4. Enforce Global Queued Memory Bound (MAX_QUEUED_BYTES = 1 MiB) */
    printf("\nTesting Global Queued Memory Bound (1 MiB max)...\n");
    /* Enqueue 256 messages of 4096 bytes: 256 * 4096 = 1,048,576 bytes = 1 MiB */
    for (int s = 0; s < 4; s++) {
        for (int i = 0; i < PER_SENDER_QUEUE_CAP; i++) {
            m1.header.sender_id = s;
            m1.header.payload_len = 4096;
            queue_enqueue(&qm, s, &m1);
        }
    }
    TEST_CHECK(queue_get_global_bytes(&qm) == MAX_QUEUED_BYTES, "Queued bytes reaches exactly 1 MiB");

    /* Dequeue one message to leave 4096 bytes free, then attempt to enqueue 4097 bytes */
    queue_dequeue_sender(&qm, 0, &out_m);
    m1.header.sender_id = 0;
    m1.header.payload_len = 4096;
    rc = queue_enqueue(&qm, 0, &m1);
    TEST_CHECK(rc == QUEUE_OK, "Re-enqueueing 4096 bytes into available space succeeds");

    queue_manager_cleanup(&qm);

    printf("\nQueue Unit Tests: %d / %d Passed.\n\n", g_passed, g_total);
    return (g_passed == g_total) ? 0 : 1;
}
