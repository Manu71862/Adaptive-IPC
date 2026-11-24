/**
 * queue.h — Bounded Multi-Queue Storage & Hard Limit Enforcement
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Implements per-sender bounded FIFO queues and global payload tracking.
 * Enforces hard architectural limits independently of any dynamic controller.
 */
#ifndef QUEUE_H
#define QUEUE_H

#include "ipc_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QUEUE_OK               = 0,
    QUEUE_ERR_INVALID      = -1,
    QUEUE_ERR_SENDER_FULL  = -2,
    QUEUE_ERR_GLOBAL_FULL  = -3,
    QUEUE_ERR_BYTES_FULL   = -4,
    QUEUE_ERR_EMPTY        = -5
} QueueResult;

/** Node inside per-sender queue */
typedef struct QueueNode {
    IpcMessage        msg;
    struct QueueNode *next;
} QueueNode;

/** Per-sender queue container */
typedef struct {
    QueueNode *head;
    QueueNode *tail;
    size_t     count;           /* Current messages in sender queue */
    size_t     queued_bytes;    /* Payload bytes in sender queue */
    size_t     total_enqueued;  /* Cumulative count */
    size_t     total_dequeued;  /* Cumulative count */
    size_t     total_dropped;   /* Cumulative dropped due to limits */
} SenderQueue;

/** Global broker multi-queue manager */
typedef struct {
    SenderQueue queues[MAX_SENDERS];
    size_t      global_count;        /* Total messages currently queued across all senders */
    size_t      global_queued_bytes; /* Total payload bytes currently queued */
    size_t      peak_count;          /* Peak queue depth observed */
    size_t      peak_bytes;          /* Peak payload memory observed */
    size_t      total_enqueued;      /* Total admitted messages */
    size_t      total_dequeued;      /* Total dispatched messages */
    size_t      total_dropped;       /* Total rejected messages */
} BrokerQueueManager;

/* ── Queue Management Operations ─────────────────────────────────── */

void queue_manager_init(BrokerQueueManager *qm);
void queue_manager_cleanup(BrokerQueueManager *qm);

/**
 * Enqueues a message for a given sender, enforcing:
 *  1. Per-sender message capacity (<= 64)
 *  2. Global broker message capacity (<= 256)
 *  3. Global queued payload memory (<= 1 MiB)
 */
QueueResult queue_enqueue(BrokerQueueManager *qm, int sender_id, const IpcMessage *msg);

/** Dequeues the head message from a specific sender's queue (used by DRR) */
QueueResult queue_dequeue_sender(BrokerQueueManager *qm, int sender_id, IpcMessage *out_msg);

/** Peeks at the head message of a sender's queue without removing it */
QueueResult queue_peek_sender(const BrokerQueueManager *qm, int sender_id, const IpcMessage **out_msg);

/** Returns the payload length of the head message for a sender (for DRR checks) */
int queue_peek_head_len(const BrokerQueueManager *qm, int sender_id);

/** Clears all queued messages for a disconnecting sender, reclaiming capacity */
void queue_clear_sender(BrokerQueueManager *qm, int sender_id);

/** Checks if a sender's queue has pending messages */
bool queue_sender_has_messages(const BrokerQueueManager *qm, int sender_id);

/** Returns current total queued message count across all senders */
size_t queue_get_global_count(const BrokerQueueManager *qm);

/** Returns current total queued payload bytes across all senders */
size_t queue_get_global_bytes(const BrokerQueueManager *qm);

/** Returns current queue utilization ratio [0.0 .. 1.0] */
double queue_get_utilization(const BrokerQueueManager *qm);

#ifdef __cplusplus
}
#endif

#endif /* QUEUE_H */
