/**
 * ipc_protocol.h — Binary Wire Protocol & Architectural Constants
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Defines the frame header, message types, resource bounds,
 * and wire serialization utilities for UNIX domain socket communication.
 */
#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Protocol Identification ─────────────────────────────────────── */
#define IPC_MAGIC           0x49504350U /* "IPCP" */
#define IPC_VERSION         1

/* ── Resource Management Bounds ──────────────────────────────────── */
#define MAX_SENDERS             8       /* Maximum concurrent sender clients */
#define MAX_MSG_PAYLOAD         4096    /* 4 KiB maximum payload per message */
#define GLOBAL_QUEUE_CAP        256     /* Global broker queue capacity (messages) */
#define PER_SENDER_QUEUE_CAP    64      /* Per-sender pending queue capacity */
#define MAX_QUEUED_BYTES        1048576 /* 1 MiB hard queued-payload limit */
#define CONTROLLER_INTERVAL_MS  50      /* Adaptive controller update interval */

/* ── Default Socket Paths ────────────────────────────────────────── */
#define DEFAULT_BROKER_SOCKET   "/tmp/adaptive_ipc_broker.sock"
#define DEFAULT_RECEIVER_SOCKET "/tmp/adaptive_ipc_receiver.sock"

/* ── Message Types ───────────────────────────────────────────────── */
typedef enum {
    MSG_NONE     = 0,
    MSG_REGISTER = 1,   /* Sender registers connection with broker */
    MSG_CREDIT   = 2,   /* Broker grants transmission credits to sender */
    MSG_DATA     = 3,   /* Sender submits data payload to broker */
    MSG_ACK      = 4,   /* Receiver acknowledges completed processing */
    MSG_STOP     = 5    /* Clean shutdown notification */
} IpcMsgType;

/* ── Protocol Frame Header (40 bytes, packed) ─────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /* IPC_MAGIC */
    uint16_t version;           /* IPC_VERSION */
    uint16_t msg_type;          /* IpcMsgType (DATA, CREDIT, ACK, etc.) */
    int32_t  sender_id;         /* Sender index [0 .. MAX_SENDERS-1] */
    uint64_t msg_seq;           /* Monotonic message sequence number */
    uint64_t timestamp_ns;      /* Monotonic timestamp in nanoseconds */
    uint32_t payload_len;       /* Payload length (0 .. MAX_MSG_PAYLOAD) */
    uint32_t credit_val;        /* Credits granted in MSG_CREDIT */
    uint32_t checksum;          /* Integrity checksum over payload */
} IpcHeader;

/* ── In-Memory Message Container ─────────────────────────────────── */
typedef struct {
    IpcHeader header;
    uint8_t   payload[MAX_MSG_PAYLOAD];
} IpcMessage;

/* ── Utility Functions ───────────────────────────────────────────── */

/** Computes FNV-1a checksum over payload buffer for message integrity */
static inline uint32_t ipc_checksum(const void *data, size_t len) {
    uint32_t hash = 2166136261u;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

/** Formats a human-readable name for a message type */
static inline const char *ipc_msg_type_str(uint16_t type) {
    switch (type) {
        case MSG_REGISTER: return "REGISTER";
        case MSG_CREDIT:   return "CREDIT";
        case MSG_DATA:     return "DATA";
        case MSG_ACK:      return "ACK";
        case MSG_STOP:     return "STOP";
        default:           return "UNKNOWN";
    }
}

/** Validates message header invariants and payload integrity */
static inline bool ipc_msg_validate(const IpcMessage *msg) {
    if (!msg) return false;
    if (msg->header.magic != IPC_MAGIC) return false;
    if (msg->header.version != IPC_VERSION) return false;
    if (msg->header.payload_len > MAX_MSG_PAYLOAD) return false;
    if (msg->header.sender_id < -1 || msg->header.sender_id >= MAX_SENDERS) return false;

    if (msg->header.payload_len > 0) {
        uint32_t sum = ipc_checksum(msg->payload, msg->header.payload_len);
        if (sum != msg->header.checksum) return false;
    }
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* IPC_PROTOCOL_H */
