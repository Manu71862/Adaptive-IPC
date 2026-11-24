/**
 * receiver.c — IPC Receiver Process with Latency Tracking & Stall Simulation
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Implements:
 *  - Message integrity checking and checksum verification
 *  - High-precision end-to-end latency sampling (median, p95, p99)
 *  - Simulated processing delays and programmable receiver stalls
 *  - Immediate completion ACK feedback to broker
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>

#include "common.h"
#include "ipc_protocol.h"

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[]) {
    const char *socket_path = DEFAULT_BROKER_SOCKET;
    int max_messages = 0; /* 0 = run until broker STOP */
    int proc_delay_us = 0;
    int stall_after_msgs = 0;
    int stall_duration_ms = 0;
    const char *csv_out = NULL;
    int expected_flow_count = 0; /* 0 = infer from senders that delivered data (legacy) */

    int opt;
    while ((opt = getopt(argc, argv, "k:n:d:s:t:o:f:vh")) != -1) {
        switch (opt) {
            case 'k': socket_path = optarg; break;
            case 'n': max_messages = atoi(optarg); break;
            case 'd': proc_delay_us = atoi(optarg); break;
            case 's': stall_after_msgs = atoi(optarg); break;
            case 't': stall_duration_ms = atoi(optarg); break;
            case 'o': csv_out = optarg; break;
            case 'f': expected_flow_count = atoi(optarg); break;
            case 'v': log_set_level(LOG_LEVEL_DEBUG); break;
            case 'h':
            default:
                fprintf(stderr, "Usage: %s [-k socket] [-n max_msgs] [-d proc_delay_us] [-s stall_after] [-t stall_ms] [-o out.csv] [-f flow_count]\n", argv[0]);
                return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    LOG_I("Receiver process connecting to broker '%s'...", socket_path);

    int fd = ipc_connect_socket(socket_path);
    if (fd < 0) {
        LOG_E("Failed to connect to broker: %s", strerror(-fd));
        return 1;
    }

    /* Register as receiver (sender_id = -1) */
    IpcMessage reg_msg;
    memset(&reg_msg, 0, sizeof(reg_msg));
    reg_msg.header.magic = IPC_MAGIC;
    reg_msg.header.version = IPC_VERSION;
    reg_msg.header.msg_type = MSG_REGISTER;
    reg_msg.header.sender_id = -1;
    reg_msg.header.timestamp_ns = get_time_ns();

    if (ipc_send_msg(fd, &reg_msg) <= 0) {
        LOG_E("Failed to send receiver registration");
        close(fd);
        return 1;
    }

    LOG_I("Receiver registered successfully. Processing messages...");

    LatencySampleBuffer *sb = sample_buffer_create(2048);
    uint64_t total_received_msgs = 0;
    uint64_t total_received_bytes = 0;
    uint64_t integrity_errors = 0;

    uint64_t per_sender_msgs[MAX_SENDERS] = {0};
    uint64_t per_sender_bytes[MAX_SENDERS] = {0};

    uint64_t start_time_ns = get_time_ns();
    bool stall_triggered = false;

    while (g_running) {
        if (max_messages > 0 && total_received_msgs >= (uint64_t)max_messages) {
            break;
        }

        /* Check for programmed stall injection */
        if (!stall_triggered && stall_after_msgs > 0 && total_received_msgs >= (uint64_t)stall_after_msgs) {
            LOG_W("Simulating receiver slowdown/stall: sleeping for %d ms...", stall_duration_ms);
            sleep_ms(stall_duration_ms);
            stall_triggered = true;
            LOG_I("Receiver recovered from stall. Resuming normal processing.");
        }

        IpcMessage msg;
        int r = ipc_recv_msg(fd, &msg);
        if (r > 0) {
            if (msg.header.msg_type == MSG_STOP) {
                LOG_I("Broker signaled STOP. Terminating receiver loop.");
                break;
            }

            if (msg.header.msg_type == MSG_DATA) {
                uint64_t now_ns = get_time_ns();
                double latency_us = (now_ns > msg.header.timestamp_ns) ?
                                    (double)(now_ns - msg.header.timestamp_ns) * 1e-3 : 0.0;
                sample_buffer_add(sb, latency_us);

                /* Verify checksum integrity */
                if (msg.header.payload_len > 0) {
                    uint32_t calc_sum = ipc_checksum(msg.payload, msg.header.payload_len);
                    if (calc_sum != msg.header.checksum) {
                        integrity_errors++;
                        LOG_E("Data integrity error detected! Sender=%d Seq=%lu",
                              msg.header.sender_id, (unsigned long)msg.header.msg_seq);
                    }
                }

                total_received_msgs++;
                total_received_bytes += (sizeof(IpcHeader) + msg.header.payload_len);

                int sid = msg.header.sender_id;
                if (sid >= 0 && sid < MAX_SENDERS) {
                    per_sender_msgs[sid]++;
                    per_sender_bytes[sid] += (sizeof(IpcHeader) + msg.header.payload_len);
                }

                if (proc_delay_us > 0) {
                    usleep((useconds_t)proc_delay_us);
                }

                /* Send completion ACK back to broker */
                IpcMessage ack_msg;
                memset(&ack_msg, 0, sizeof(ack_msg));
                ack_msg.header.magic = IPC_MAGIC;
                ack_msg.header.version = IPC_VERSION;
                ack_msg.header.msg_type = MSG_ACK;
                ack_msg.header.sender_id = msg.header.sender_id;
                ack_msg.header.msg_seq = msg.header.msg_seq;
                ack_msg.header.timestamp_ns = get_time_ns();
                ipc_send_msg(fd, &ack_msg);
            }
        } else if (r == 0 || (r < 0 && r != -EAGAIN)) {
            LOG_I("Broker connection closed.");
            break;
        }
    }

    close(fd);

    uint64_t end_time_ns = get_time_ns();
    double total_runtime_s = (double)(end_time_ns - start_time_ns) * 1e-9;
    double throughput_mps = (total_runtime_s > 0) ? (double)total_received_msgs / total_runtime_s : 0;
    double throughput_mb = (total_runtime_s > 0) ? ((double)total_received_bytes / (1024.0 * 1024.0)) / total_runtime_s : 0;

    sample_buffer_sort(sb);
    double p50_lat = sample_buffer_percentile(sb, 50.0);
    double p95_lat = sample_buffer_percentile(sb, 95.0);
    double p99_lat = sample_buffer_percentile(sb, 99.0);
    double mean_lat = sample_buffer_mean(sb);

    /*
     * Fairness must be computed over ALL n participating flows, including any
     * that delivered zero bytes.  The caller advertises the flow count via -f;
     * if omitted we count senders whose slot was touched (msgs or bytes != 0)
     * and fall back to MAX_SENDERS as a conservative upper bound.
     *
     * Any sender with index < n_flows but 0 delivered bytes is included as 0.0
     * so that J = 0.5 for a 2-flow system where one received nothing.
     */
    int n_flows = expected_flow_count;
    if (n_flows <= 0) {
        /* Infer: find the highest sender slot that received any message */
        n_flows = 0;
        for (int i = 0; i < MAX_SENDERS; i++) {
            if (per_sender_bytes[i] > 0 || per_sender_msgs[i] > 0) n_flows = i + 1;
        }
        if (n_flows < 1) n_flows = 1;
    }
    if (n_flows > MAX_SENDERS) n_flows = MAX_SENDERS;

    double flow_bytes[MAX_SENDERS];
    for (int i = 0; i < n_flows; i++) {
        flow_bytes[i] = (double)per_sender_bytes[i];
    }
    double fairness = calculate_jains_fairness(flow_bytes, (size_t)n_flows);

    printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    RECEIVER RUNTIME SUMMARY                      ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  Total Received    : %-8lu messages                              ║\n", (unsigned long)total_received_msgs);
    printf("║  Total Data Volume : %8.2f MiB                                  ║\n", (double)total_received_bytes / (1024.0 * 1024.0));
    printf("║  Elapsed Time      : %8.3f seconds                               ║\n", total_runtime_s);
    printf("║  Throughput        : %8.2f msgs/s  (%6.2f MB/s)                 ║\n", throughput_mps, throughput_mb);
    printf("║  Median Latency p50: %8.2f µs                                    ║\n", p50_lat);
    printf("║  Tail Latency p95  : %8.2f µs                                    ║\n", p95_lat);
    printf("║  Tail Latency p99  : %8.2f µs                                    ║\n", p99_lat);
    printf("║  Mean Latency      : %8.2f µs                                    ║\n", mean_lat);
    printf("║  Integrity Errors  : %-8lu                                       ║\n", (unsigned long)integrity_errors);
    printf("║  Jain Byte Fairness: %8.4f  (over %d flows)                     ║\n", fairness, n_flows);
    for (int i = 0; i < n_flows; i++) {
        printf("║    Sender %d delivered : %8lu msgs  %10lu bytes               ║\n",
               i, (unsigned long)per_sender_msgs[i], (unsigned long)per_sender_bytes[i]);
    }
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    if (csv_out) {
        FILE *cf = fopen(csv_out, "w");
        if (cf) {
            /* Header: base metrics + per-sender columns (s0_msgs,s0_bytes,...,sN_msgs,sN_bytes) */
            fprintf(cf, "received_msgs,volume_bytes,runtime_s,throughput_mps,throughput_mb,"
                        "p50_us,p95_us,p99_us,mean_lat_us,integrity_errors,jain_fairness,n_flows");
            for (int i = 0; i < n_flows; i++) {
                fprintf(cf, ",s%d_msgs,s%d_bytes", i, i);
            }
            fprintf(cf, "\n");

            fprintf(cf, "%lu,%lu,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu,%.4f,%d",
                    (unsigned long)total_received_msgs, (unsigned long)total_received_bytes,
                    total_runtime_s, throughput_mps, throughput_mb,
                    p50_lat, p95_lat, p99_lat, mean_lat,
                    (unsigned long)integrity_errors, fairness, n_flows);
            for (int i = 0; i < n_flows; i++) {
                fprintf(cf, ",%lu,%lu",
                        (unsigned long)per_sender_msgs[i],
                        (unsigned long)per_sender_bytes[i]);
            }
            fprintf(cf, "\n");
            fclose(cf);
            LOG_I("Receiver metrics written to '%s'", csv_out);
        }
    }

    sample_buffer_free(sb);
    return (integrity_errors == 0) ? 0 : 1;
}
