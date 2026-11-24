/**
 * sender.c — Configurable IPC Sender Client with Credit-Based Flow Control
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Implements:
 *  - Credit accounting: only transmits DATA when credit budget > 0
 *  - Configurable workloads: message payload size (up to 4 KiB), target rate, bursts
 *  - Sender waiting-time tracking (time stalled waiting for credits)
 *  - Optional protocol violation mode (--rogue) for robustness verification
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <poll.h>

#include "common.h"
#include "ipc_protocol.h"

int main(int argc, char *argv[]) {
    int sender_id = 0;
    int num_messages = 200;
    size_t payload_len = 1024;
    int target_rate_mps = 0; /* 0 = max unthrottled burst */
    int burst_size = 1;
    int burst_delay_ms = 0;
    bool rogue_mode = false;
    const char *socket_path = DEFAULT_BROKER_SOCKET;
    int sync_fd = -1;
    int ready_fd = -1;

    int opt;
    while ((opt = getopt(argc, argv, "i:n:s:r:b:d:k:w:R:Xvh")) != -1) {
        switch (opt) {
            case 'i': sender_id = atoi(optarg); break;
            case 'n': num_messages = atoi(optarg); break;
            case 's': payload_len = (size_t)atoi(optarg); break;
            case 'r': target_rate_mps = atoi(optarg); break;
            case 'b': burst_size = atoi(optarg); break;
            case 'd': burst_delay_ms = atoi(optarg); break;
            case 'k': socket_path = optarg; break;
            case 'w': sync_fd = atoi(optarg); break;
            case 'R': ready_fd = atoi(optarg); break;
            case 'X': rogue_mode = true; break; /* Rogue mode violates credits */
            case 'v': log_set_level(LOG_LEVEL_DEBUG); break;
            case 'h':
            default:
                fprintf(stderr, "Usage: %s -i <id> [-n <msgs>] [-s <bytes>] [-r <rate>] [-b <burst>] [-d <delay_ms>] [-w <sync_fd>] [-R <ready_fd>] [-X (rogue)] [-k socket]\n", argv[0]);
                return 1;
        }
    }

    if (payload_len > MAX_MSG_PAYLOAD) {
        payload_len = MAX_MSG_PAYLOAD;
    }

    LOG_I("Sender %d starting (target=%d msgs, payload=%zu bytes, rate=%d mps, rogue=%s)",
          sender_id, num_messages, payload_len, target_rate_mps, rogue_mode ? "YES" : "NO");

    int fd = ipc_connect_socket(socket_path);
    if (fd < 0) {
        LOG_E("Sender %d failed to connect to broker '%s': %s", sender_id, socket_path, strerror(-fd));
        return 1;
    }

    /* 1. Register with broker */
    IpcMessage reg_msg;
    memset(&reg_msg, 0, sizeof(reg_msg));
    reg_msg.header.magic = IPC_MAGIC;
    reg_msg.header.version = IPC_VERSION;
    reg_msg.header.msg_type = MSG_REGISTER;
    reg_msg.header.sender_id = sender_id;
    reg_msg.header.timestamp_ns = get_time_ns();

    if (ipc_send_msg(fd, &reg_msg) <= 0) {
        LOG_E("Sender %d failed to send registration", sender_id);
        close(fd);
        return 1;
    }

    uint32_t credits_available = 0;
    uint32_t total_credits_granted = 0;

    /* Wait for initial credit grant from broker so sender is armed */
    struct pollfd pfd_init;
    pfd_init.fd = fd;
    pfd_init.events = POLLIN;
    if (poll(&pfd_init, 1, 200) > 0 && (pfd_init.revents & POLLIN)) {
        IpcMessage cmsg;
        if (ipc_recv_msg(fd, &cmsg) > 0 && cmsg.header.msg_type == MSG_CREDIT) {
            credits_available += cmsg.header.credit_val;
            total_credits_granted += cmsg.header.credit_val;
        }
    }

    /* Signal readiness to harness if ready_fd is provided */
    if (ready_fd >= 0) {
        char rch = '1';
        ssize_t wret = write(ready_fd, &rch, 1);
        (void)wret;
        close(ready_fd);
    }

    /* 2. Deterministic Start Barrier: block until harness releases synchronization pipe */
    if (sync_fd >= 0) {
        LOG_D("Sender %d armed with %u credits; waiting on barrier fd %d...",
              sender_id, credits_available, sync_fd);
        char sync_ch;
        ssize_t sret = read(sync_fd, &sync_ch, 1);
        (void)sret;
        close(sync_fd);
        LOG_D("Sender %d released from start barrier! Starting transmission.", sender_id);
    }

    uint32_t msgs_sent = 0;

    uint64_t total_blocked_time_ns = 0;
    uint64_t start_time_ns = get_time_ns();

    /* Prepare standard payload template */
    uint8_t payload_template[MAX_MSG_PAYLOAD];
    for (size_t i = 0; i < payload_len; i++) {
        payload_template[i] = (uint8_t)((sender_id * 31 + i * 7) & 0xFF);
    }
    uint32_t payload_chk = ipc_checksum(payload_template, payload_len);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    int burst_counter = 0;

    while (msgs_sent < (uint32_t)num_messages) {
        /* Check for inbound CREDIT or STOP messages from broker */
        int p_timeout = (credits_available > 0 || rogue_mode) ? 0 : 50; /* Poll with 50ms if waiting for credits */
        int pret = poll(&pfd, 1, p_timeout);
        if (pret > 0 && (pfd.revents & POLLIN)) {
            IpcMessage in_msg;
            int r = ipc_recv_msg(fd, &in_msg);
            if (r > 0) {
                if (in_msg.header.msg_type == MSG_CREDIT) {
                    credits_available += in_msg.header.credit_val;
                    total_credits_granted += in_msg.header.credit_val;
                    LOG_D("Sender %d: +%u credits received (total avail=%u)",
                          sender_id, in_msg.header.credit_val, credits_available);
                } else if (in_msg.header.msg_type == MSG_STOP) {
                    LOG_W("Sender %d: Broker requested STOP", sender_id);
                    break;
                }
            } else if (r == 0 || (r < 0 && r != -EAGAIN)) {
                LOG_W("Sender %d: Broker closed connection", sender_id);
                break;
            }
        }

        /* If not in rogue mode and no credits available, block & record waiting time */
        if (!rogue_mode && credits_available == 0) {
            uint64_t block_start = get_time_ns();
            int pret2 = poll(&pfd, 1, 50);
            uint64_t block_end = get_time_ns();
            total_blocked_time_ns += (block_end - block_start);

            if (pret2 > 0 && (pfd.revents & POLLIN)) {
                IpcMessage in_msg;
                int r = ipc_recv_msg(fd, &in_msg);
                if (r > 0 && in_msg.header.msg_type == MSG_CREDIT) {
                    credits_available += in_msg.header.credit_val;
                    total_credits_granted += in_msg.header.credit_val;
                }
            }
            continue;
        }

        /* Transmit DATA message */
        IpcMessage data_msg;
        memset(&data_msg, 0, sizeof(data_msg));
        data_msg.header.magic = IPC_MAGIC;
        data_msg.header.version = IPC_VERSION;
        data_msg.header.msg_type = MSG_DATA;
        data_msg.header.sender_id = sender_id;
        data_msg.header.msg_seq = msgs_sent + 1;
        data_msg.header.timestamp_ns = get_time_ns();
        data_msg.header.payload_len = (uint32_t)payload_len;
        data_msg.header.checksum = payload_chk;
        memcpy(data_msg.payload, payload_template, payload_len);

        int sent = ipc_send_msg(fd, &data_msg);
        if (sent <= 0) {
            LOG_E("Sender %d: failed to send DATA message (code=%d)", sender_id, sent);
            break;
        }

        msgs_sent++;
        if (credits_available > 0) {
            credits_available--;
        }

        burst_counter++;
        if (burst_size > 0 && burst_counter >= burst_size) {
            burst_counter = 0;
            if (burst_delay_ms > 0) sleep_ms(burst_delay_ms);
        }

        /* Rate limiter if requested */
        if (target_rate_mps > 0) {
            int delay_us = 1000000 / target_rate_mps;
            usleep((useconds_t)delay_us);
        }
    }

    /* Drain any inbound CREDIT messages before sending STOP */
    IpcMessage drain_msg;
    while (recv(fd, &drain_msg, sizeof(drain_msg), MSG_DONTWAIT) > 0) {}

    /* Send clean STOP message */
    IpcMessage stop_msg;
    memset(&stop_msg, 0, sizeof(stop_msg));
    stop_msg.header.magic = IPC_MAGIC;
    stop_msg.header.version = IPC_VERSION;
    stop_msg.header.msg_type = MSG_STOP;
    stop_msg.header.sender_id = sender_id;
    stop_msg.header.timestamp_ns = get_time_ns();
    ipc_send_msg(fd, &stop_msg);

    /* Half-close write side so FIN is sent and kernel flushes outgoing buffer */
    shutdown(fd, SHUT_WR);

    /* Wait for broker to consume STOP and close connection (EOF) */
    while (recv(fd, &drain_msg, sizeof(drain_msg), 0) > 0) {}

    close(fd);

    double total_time_s = (double)(get_time_ns() - start_time_ns) * 1e-9;
    double blocked_time_s = (double)total_blocked_time_ns * 1e-9;

    printf("[Sender %d Summary] Sent: %u/%d msgs (%zu bytes/msg) | Credits Granted: %u | Total Time: %.3fs | Stall/Blocked Time: %.3fs (%.1f%%)\n",
           sender_id, msgs_sent, num_messages, payload_len, total_credits_granted,
           total_time_s, blocked_time_s,
           (total_time_s > 0) ? (blocked_time_s / total_time_s) * 100.0 : 0.0);

    return 0;
}
