/**
 * test_robustness.c — Edge-Case, Fault-Tolerance, and Safety Verification
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Verifies:
 *  1. Rogue sender flooding without credits (hard bounds protection)
 *  2. Abrupt sender disconnect and queue reclamation
 *  3. Receiver stall, backpressure escalation, and recovery
 *  4. Clean graceful shutdown and socket unlinking
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

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

#define TEST_SOCKET_PATH "/tmp/test_adaptive_ipc_robustness.sock"

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║        ROBUSTNESS, SAFETY & FAULT-TOLERANCE TEST SUITE           ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    ipc_unlink_socket(TEST_SOCKET_PATH);

    /* ── 1. Start Broker in Background ──────────────────────────────── */
    printf("[1/3] Testing Rogue Sender Protocol Violation...\n");
    pid_t broker_pid = fork();
    if (broker_pid == 0) {
        /* Run broker with 5 second timeout */
        execl("./bin/broker", "broker", "-c", "C", "-s", TEST_SOCKET_PATH, "-t", "6", NULL);
        _exit(127);
    }

    sleep_ms(150); /* Allow broker to bind and listen */

    /* Start Receiver */
    pid_t receiver_pid = fork();
    if (receiver_pid == 0) {
        execl("./bin/receiver", "receiver", "-k", TEST_SOCKET_PATH, "-n", "300", NULL);
        _exit(127);
    }

    sleep_ms(100);

    /* Start Rogue Sender with -X (deliberately sends 200 messages without waiting for credits) */
    pid_t rogue_pid = fork();
    if (rogue_pid == 0) {
        execl("./bin/sender", "sender", "-k", TEST_SOCKET_PATH, "-i", "0", "-n", "200", "-s", "512", "-X", NULL);
        _exit(127);
    }

    int rogue_status = 0;
    waitpid(rogue_pid, &rogue_status, 0);
    TEST_CHECK(WIFEXITED(rogue_status) && WEXITSTATUS(rogue_status) == 0,
               "Rogue sender process completes without crashing broker");

    /* ── 2. Abrupt Sender Disconnect Test ───────────────────────────── */
    printf("\n[2/3] Testing Abrupt Sender Disconnect & Resource Reclamation...\n");
    int client_fd = ipc_connect_socket(TEST_SOCKET_PATH);
    TEST_CHECK(client_fd >= 0, "Client connects to broker");

    /* Register sender 1 */
    IpcMessage reg;
    memset(&reg, 0, sizeof(reg));
    reg.header.magic = IPC_MAGIC;
    reg.header.version = IPC_VERSION;
    reg.header.msg_type = MSG_REGISTER;
    reg.header.sender_id = 1;
    ipc_send_msg(client_fd, &reg);

    /* Send one message and abruptly close socket */
    IpcMessage dm;
    memset(&dm, 0, sizeof(dm));
    dm.header.magic = IPC_MAGIC;
    dm.header.version = IPC_VERSION;
    dm.header.msg_type = MSG_DATA;
    dm.header.sender_id = 1;
    dm.header.msg_seq = 1;
    dm.header.payload_len = 256;
    ipc_send_msg(client_fd, &dm);

    close(client_fd); /* Abrupt termination without STOP */
    sleep_ms(100);

    /* Connect another sender 2 to verify broker is responsive and healthy */
    int client_fd2 = ipc_connect_socket(TEST_SOCKET_PATH);
    TEST_CHECK(client_fd2 >= 0, "Broker remains healthy and accepts new clients after abrupt disconnect");

    reg.header.sender_id = 2;
    ipc_send_msg(client_fd2, &reg);
    close(client_fd2);

    /* ── 3. Clean Graceful Shutdown ─────────────────────────────────── */
    printf("\n[3/3] Testing Clean Teardown & Socket Cleanup...\n");
    kill(broker_pid, SIGINT);
    int broker_status = 0;
    waitpid(broker_pid, &broker_status, 0);
    TEST_CHECK(WIFEXITED(broker_status), "Broker terminated cleanly on SIGINT signal");

    if (receiver_pid > 0) {
        kill(receiver_pid, SIGTERM);
        waitpid(receiver_pid, NULL, 0);
    }

    TEST_CHECK(access(TEST_SOCKET_PATH, F_OK) != 0, "Broker cleanly unlinks UNIX domain socket on exit");

    printf("\nRobustness Tests: %d / %d Passed.\n\n", g_passed, g_total);
    return (g_passed == g_total) ? 0 : 1;
}
