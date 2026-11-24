/**
 * scheduler.c — Implementation of FIFO and Deficit Round-Robin (DRR) Dispatch
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 */
#include "scheduler.h"
#include "common.h"
#include <string.h>

void scheduler_init(BrokerScheduler *sched, DispatchPolicy policy) {
    if (!sched) return;
    memset(sched, 0, sizeof(BrokerScheduler));
    sched->policy = policy;
    sched->quantum_bytes = DRR_DEFAULT_QUANTUM_BYTES;
    sched->current_rr_index = 0;
}

void scheduler_reset_sender(BrokerScheduler *sched, int sender_id) {
    if (!sched || sender_id < 0 || sender_id >= MAX_SENDERS) return;
    sched->deficit_counter[sender_id] = 0;
}

static QueueResult dispatch_fifo(BrokerScheduler *sched, BrokerQueueManager *qm,
                                 IpcMessage *out_msg, int *out_sender_id)
{
    int oldest_sender = -1;
    uint64_t oldest_ts = UINT64_MAX;

    for (int i = 0; i < MAX_SENDERS; i++) {
        const IpcMessage *peek = NULL;
        if (queue_peek_sender(qm, i, &peek) == QUEUE_OK && peek) {
            if (peek->header.timestamp_ns < oldest_ts) {
                oldest_ts = peek->header.timestamp_ns;
                oldest_sender = i;
            }
        }
    }

    if (oldest_sender < 0) {
        return QUEUE_ERR_EMPTY;
    }

    QueueResult rc = queue_dequeue_sender(qm, oldest_sender, out_msg);
    if (rc == QUEUE_OK) {
        size_t bytes = sizeof(IpcHeader) + out_msg->header.payload_len;
        sched->dispatched_msgs[oldest_sender]++;
        sched->dispatched_bytes[oldest_sender] += bytes;
        sched->total_dispatched_msgs++;
        sched->total_dispatched_bytes += bytes;
        if (out_sender_id) *out_sender_id = oldest_sender;
    }
    return rc;
}

static QueueResult dispatch_drr(BrokerScheduler *sched, BrokerQueueManager *qm,
                                IpcMessage *out_msg, int *out_sender_id)
{
    /* Check if any queue has messages */
    bool any_messages = false;
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (queue_sender_has_messages(qm, i)) {
            any_messages = true;
            break;
        }
    }
    if (!any_messages) return QUEUE_ERR_EMPTY;

    for (int steps = 0; steps < MAX_SENDERS * 2; steps++) {
        int i = sched->current_rr_index;

        if (!queue_sender_has_messages(qm, i)) {
            sched->deficit_counter[i] = 0;
            sched->current_rr_index = (i + 1) % MAX_SENDERS;
            continue;
        }

        int head_payload = queue_peek_head_len(qm, i);
        if (head_payload < 0) {
            sched->current_rr_index = (i + 1) % MAX_SENDERS;
            continue;
        }
        size_t msg_total_bytes = sizeof(IpcHeader) + (size_t)head_payload;

        /* If deficit is insufficient for the head packet, replenish quantum */
        if (sched->deficit_counter[i] < (int64_t)msg_total_bytes) {
            sched->deficit_counter[i] += sched->quantum_bytes;
        }

        /* If deficit is sufficient, dequeue and transmit */
        if (sched->deficit_counter[i] >= (int64_t)msg_total_bytes) {
            QueueResult rc = queue_dequeue_sender(qm, i, out_msg);
            if (rc == QUEUE_OK) {
                sched->deficit_counter[i] -= (int64_t)msg_total_bytes;
                sched->dispatched_msgs[i]++;
                sched->dispatched_bytes[i] += msg_total_bytes;
                sched->total_dispatched_msgs++;
                sched->total_dispatched_bytes += msg_total_bytes;

                if (out_sender_id) *out_sender_id = i;

                /* If queue has become empty, reset deficit and advance round */
                if (!queue_sender_has_messages(qm, i)) {
                    sched->deficit_counter[i] = 0;
                    sched->current_rr_index = (i + 1) % MAX_SENDERS;
                } else {
                    /* Check if remaining deficit can cover the next packet */
                    int next_payload = queue_peek_head_len(qm, i);
                    if (next_payload >= 0 &&
                        sched->deficit_counter[i] < (int64_t)(sizeof(IpcHeader) + (size_t)next_payload)) {
                        /* Deficit exhausted for this round; advance round-robin index */
                        sched->current_rr_index = (i + 1) % MAX_SENDERS;
                    }
                    /* If deficit can cover next packet, leave sched->current_rr_index unchanged */
                }

                return QUEUE_OK;
            }
        }

        /* If still cannot transmit after replenishment, advance */
        sched->current_rr_index = (i + 1) % MAX_SENDERS;
    }

    return QUEUE_ERR_EMPTY;
}

QueueResult scheduler_dispatch_next(BrokerScheduler *sched, BrokerQueueManager *qm,
                                    IpcMessage *out_msg, int *out_sender_id)
{
    if (!sched || !qm || !out_msg) return QUEUE_ERR_INVALID;

    if (sched->policy == DISPATCH_DRR) {
        return dispatch_drr(sched, qm, out_msg, out_sender_id);
    } else {
        return dispatch_fifo(sched, qm, out_msg, out_sender_id);
    }
}

double scheduler_get_byte_fairness(const BrokerScheduler *sched, int active_senders) {
    if (!sched || active_senders <= 0) return 1.0;
    double bytes_array[MAX_SENDERS];
    for (int i = 0; i < active_senders && i < MAX_SENDERS; i++) {
        bytes_array[i] = (double)sched->dispatched_bytes[i];
    }
    return calculate_jains_fairness(bytes_array, (size_t)active_senders);
}

double scheduler_get_msg_fairness(const BrokerScheduler *sched, int active_senders) {
    if (!sched || active_senders <= 0) return 1.0;
    double msgs_array[MAX_SENDERS];
    for (int i = 0; i < active_senders && i < MAX_SENDERS; i++) {
        msgs_array[i] = (double)sched->dispatched_msgs[i];
    }
    return calculate_jains_fairness(msgs_array, (size_t)active_senders);
}
