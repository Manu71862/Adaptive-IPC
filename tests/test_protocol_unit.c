/**
 * test_protocol_unit.c — Unit Tests for Wire Protocol & Message Serialization
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "ipc_protocol.h"
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
    printf("║          UNIT TESTS: IPC PROTOCOL & MESSAGE INTEGRITY            ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    /* 1. Header Structure Size */
    TEST_CHECK(sizeof(IpcHeader) == 40, "IpcHeader size is exactly 40 bytes packed");

    /* 2. Checksum Calculation Determinism */
    const char test_data[] = "Adaptive_Backpressure_Fairness_Control_Payload_Test";
    uint32_t c1 = ipc_checksum(test_data, strlen(test_data));
    uint32_t c2 = ipc_checksum(test_data, strlen(test_data));
    TEST_CHECK(c1 == c2 && c1 != 0, "Checksum calculation is deterministic and non-zero");

    /* 3. Valid Message Frame Validation */
    IpcMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.magic = IPC_MAGIC;
    msg.header.version = IPC_VERSION;
    msg.header.msg_type = MSG_DATA;
    msg.header.sender_id = 2;
    msg.header.msg_seq = 101;
    msg.header.payload_len = (uint32_t)strlen(test_data);
    msg.header.checksum = c1;
    memcpy(msg.payload, test_data, strlen(test_data));

    TEST_CHECK(ipc_msg_validate(&msg), "Valid message passes integrity validation");

    /* 4. Corrupted Magic Detection */
    msg.header.magic = 0xBADF00D;
    TEST_CHECK(!ipc_msg_validate(&msg), "Corrupted magic number is rejected");
    msg.header.magic = IPC_MAGIC;

    /* 5. Incompatible Version Detection */
    msg.header.version = 99;
    TEST_CHECK(!ipc_msg_validate(&msg), "Incompatible protocol version is rejected");
    msg.header.version = IPC_VERSION;

    /* 6. Oversized Payload Detection */
    msg.header.payload_len = MAX_MSG_PAYLOAD + 1;
    TEST_CHECK(!ipc_msg_validate(&msg), "Payload exceeding MAX_MSG_PAYLOAD is rejected");
    msg.header.payload_len = (uint32_t)strlen(test_data);

    /* 7. Invalid Sender ID Detection */
    msg.header.sender_id = MAX_SENDERS + 2;
    TEST_CHECK(!ipc_msg_validate(&msg), "Sender ID outside valid range is rejected");
    msg.header.sender_id = 2;

    /* 8. Tampered Payload / Checksum Mismatch Detection */
    msg.payload[5] ^= 0xFF; /* Corrupt single byte */
    TEST_CHECK(!ipc_msg_validate(&msg), "Tampered payload with invalid checksum is rejected");
    msg.payload[5] ^= 0xFF; /* Restore byte */
    TEST_CHECK(ipc_msg_validate(&msg), "Restored payload passes checksum validation");

    /* 9. Zero-Payload Control Frame */
    IpcMessage ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.header.magic = IPC_MAGIC;
    ctrl.header.version = IPC_VERSION;
    ctrl.header.msg_type = MSG_CREDIT;
    ctrl.header.sender_id = 0;
    ctrl.header.credit_val = 16;
    ctrl.header.payload_len = 0;
    ctrl.header.checksum = 0;
    TEST_CHECK(ipc_msg_validate(&ctrl), "Zero-payload control message passes validation");

    printf("\nProtocol Unit Tests: %d / %d Passed.\n\n", g_passed, g_total);
    return (g_passed == g_total) ? 0 : 1;
}
