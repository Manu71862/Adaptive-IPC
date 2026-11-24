/**
 * scheduler.h — Dispatch Schedulers: Global FIFO & Deficit Round-Robin (DRR)
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Implements:
 *  1. Strict FIFO dispatch based on ingress timestamps.
 *  2. Deficit Round-Robin (DRR) fair scheduling accounting for message byte sizes.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "queue.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPATCH_FIFO = 0,
    DISPATCH_DRR  = 1
} DispatchPolicy;

#define DRR_DEFAULT_QUANTUM_BYTES (MAX_MSG_PAYLOAD + sizeof(IpcHeader))

typedef struct {
    DispatchPolicy policy;
    uint32_t       quantum_bytes;

    /* DRR State per sender */
    int64_t        deficit_counter[MAX_SENDERS];
    int            current_rr_index;

    /* Scheduling Metrics */
    uint64_t       dispatched_msgs[MAX_SENDERS];
    uint64_t       dispatched_bytes[MAX_SENDERS];
    uint64_t       total_dispatched_msgs;
    uint64_t       total_dispatched_bytes;
} BrokerScheduler;

/* ── Scheduler API ───────────────────────────────────────────────── */

void scheduler_init(BrokerScheduler *sched, DispatchPolicy policy);

/**
 * Selects and dequeues the next message to dispatch to the receiver according
 * to the active scheduling discipline (FIFO or DRR).
 *
 * @param qm Pointer to the BrokerQueueManager
 * @param out_msg Buffer where dequeued message will be written
 * @param out_sender_id Pointer where chosen sender ID will be stored
 * @return QUEUE_OK if a message was dispatched, QUEUE_ERR_EMPTY if all queues empty
 */
QueueResult scheduler_dispatch_next(BrokerScheduler *sched, BrokerQueueManager *qm,
                                    IpcMessage *out_msg, int *out_sender_id);

/** Resets deficit counter for a disconnecting sender without clearing cumulative metrics */
void scheduler_reset_sender(BrokerScheduler *sched, int sender_id);

/** Computes Jain's fairness index over dispatched bytes across active senders */
double scheduler_get_byte_fairness(const BrokerScheduler *sched, int active_senders);

/** Computes Jain's fairness index over dispatched messages across active senders */
double scheduler_get_msg_fairness(const BrokerScheduler *sched, int active_senders);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_H */
