/**
 * queue.c — Implementation of Multi-Queue Storage & Hard Limit Enforcement
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 */
#include "queue.h"
#include <stdlib.h>
#include <string.h>

void queue_manager_init(BrokerQueueManager *qm) {
    if (!qm) return;
    memset(qm, 0, sizeof(BrokerQueueManager));
}

void queue_manager_cleanup(BrokerQueueManager *qm) {
    if (!qm) return;
    for (int i = 0; i < MAX_SENDERS; i++) {
        queue_clear_sender(qm, i);
    }
    qm->global_count = 0;
    qm->global_queued_bytes = 0;
}

QueueResult queue_enqueue(BrokerQueueManager *qm, int sender_id, const IpcMessage *msg) {
    if (!qm || !msg) return QUEUE_ERR_INVALID;
    if (sender_id < 0 || sender_id >= MAX_SENDERS) return QUEUE_ERR_INVALID;

    SenderQueue *sq = &qm->queues[sender_id];
    size_t payload_sz = msg->header.payload_len;

    /* ── 1. Enforce Per-Sender Queue Bound ───────────────────────── */
    if (sq->count >= PER_SENDER_QUEUE_CAP) {
        sq->total_dropped++;
        qm->total_dropped++;
        return QUEUE_ERR_SENDER_FULL;
    }

    /* ── 2. Enforce Global Broker Queue Bound ────────────────────── */
    if (qm->global_count >= GLOBAL_QUEUE_CAP) {
        sq->total_dropped++;
        qm->total_dropped++;
        return QUEUE_ERR_GLOBAL_FULL;
    }

    /* ── 3. Enforce Global Queued Memory Bound (1 MiB) ───────────── */
    if (qm->global_queued_bytes + payload_sz > MAX_QUEUED_BYTES) {
        sq->total_dropped++;
        qm->total_dropped++;
        return QUEUE_ERR_BYTES_FULL;
    }

    /* Allocate node and link to tail */
    QueueNode *node = (QueueNode *)malloc(sizeof(QueueNode));
    if (!node) {
        sq->total_dropped++;
        qm->total_dropped++;
        return QUEUE_ERR_BYTES_FULL;
    }

    memcpy(&node->msg, msg, sizeof(IpcMessage));
    node->next = NULL;

    if (sq->tail) {
        sq->tail->next = node;
        sq->tail = node;
    } else {
        sq->head = node;
        sq->tail = node;
    }

    sq->count++;
    sq->queued_bytes += payload_sz;
    sq->total_enqueued++;

    qm->global_count++;
    qm->global_queued_bytes += payload_sz;
    qm->total_enqueued++;

    if (qm->global_count > qm->peak_count) {
        qm->peak_count = qm->global_count;
    }
    if (qm->global_queued_bytes > qm->peak_bytes) {
        qm->peak_bytes = qm->global_queued_bytes;
    }

    return QUEUE_OK;
}

QueueResult queue_dequeue_sender(BrokerQueueManager *qm, int sender_id, IpcMessage *out_msg) {
    if (!qm || !out_msg) return QUEUE_ERR_INVALID;
    if (sender_id < 0 || sender_id >= MAX_SENDERS) return QUEUE_ERR_INVALID;

    SenderQueue *sq = &qm->queues[sender_id];
    if (!sq->head || sq->count == 0) {
        return QUEUE_ERR_EMPTY;
    }

    QueueNode *node = sq->head;
    sq->head = node->next;
    if (!sq->head) {
        sq->tail = NULL;
    }

    memcpy(out_msg, &node->msg, sizeof(IpcMessage));
    size_t payload_sz = node->msg.header.payload_len;
    free(node);

    sq->count--;
    if (sq->queued_bytes >= payload_sz) {
        sq->queued_bytes -= payload_sz;
    } else {
        sq->queued_bytes = 0;
    }
    sq->total_dequeued++;

    qm->global_count--;
    if (qm->global_queued_bytes >= payload_sz) {
        qm->global_queued_bytes -= payload_sz;
    } else {
        qm->global_queued_bytes = 0;
    }
    qm->total_dequeued++;

    return QUEUE_OK;
}

QueueResult queue_peek_sender(const BrokerQueueManager *qm, int sender_id, const IpcMessage **out_msg) {
    if (!qm || !out_msg) return QUEUE_ERR_INVALID;
    if (sender_id < 0 || sender_id >= MAX_SENDERS) return QUEUE_ERR_INVALID;

    const SenderQueue *sq = &qm->queues[sender_id];
    if (!sq->head || sq->count == 0) {
        *out_msg = NULL;
        return QUEUE_ERR_EMPTY;
    }

    *out_msg = &sq->head->msg;
    return QUEUE_OK;
}

int queue_peek_head_len(const BrokerQueueManager *qm, int sender_id) {
    if (!qm || sender_id < 0 || sender_id >= MAX_SENDERS) return -1;
    const SenderQueue *sq = &qm->queues[sender_id];
    if (!sq->head || sq->count == 0) return -1;
    return (int)sq->head->msg.header.payload_len;
}

void queue_clear_sender(BrokerQueueManager *qm, int sender_id) {
    if (!qm || sender_id < 0 || sender_id >= MAX_SENDERS) return;

    SenderQueue *sq = &qm->queues[sender_id];
    QueueNode *curr = sq->head;
    while (curr) {
        QueueNode *next = curr->next;
        size_t sz = curr->msg.header.payload_len;
        if (qm->global_count > 0) qm->global_count--;
        if (qm->global_queued_bytes >= sz) qm->global_queued_bytes -= sz;
        else qm->global_queued_bytes = 0;
        free(curr);
        curr = next;
    }
    sq->head = NULL;
    sq->tail = NULL;
    sq->count = 0;
    sq->queued_bytes = 0;
}

bool queue_sender_has_messages(const BrokerQueueManager *qm, int sender_id) {
    if (!qm || sender_id < 0 || sender_id >= MAX_SENDERS) return false;
    return (qm->queues[sender_id].count > 0);
}

size_t queue_get_global_count(const BrokerQueueManager *qm) {
    return qm ? qm->global_count : 0;
}

size_t queue_get_global_bytes(const BrokerQueueManager *qm) {
    return qm ? qm->global_queued_bytes : 0;
}

double queue_get_utilization(const BrokerQueueManager *qm) {
    if (!qm || GLOBAL_QUEUE_CAP == 0) return 0.0;
    return (double)qm->global_count / (double)GLOBAL_QUEUE_CAP;
}
