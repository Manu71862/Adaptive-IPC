/**
 * broker.c — IPC Broker Daemon with Single poll() Event Loop
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Implements:
 *  - Single event loop using poll() as specified in Section 1
 *  - Per-sender queue management and hard limit enforcement (Section 3)
 *  - Adaptive backpressure controller with EWMA & hysteresis (Section 4)
 *  - Pluggable FIFO and Deficit Round-Robin (DRR) dispatching (Section 5)
 *  - Non-revocable credit allocation and global budget withholding (Section 4)
 *  - Graceful disconnect handling and clean shutdown (Section 8)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <getopt.h>

#include "common.h"
#include "ipc_protocol.h"
#include "queue.h"
#include "controller.h"
#include "scheduler.h"

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

typedef struct {
    int  fd;
    bool active;
    SenderCreditTracker tracker;
    uint64_t msgs_received;
    uint64_t bytes_received;
} SenderSlot;

static uint32_t broker_calc_sender_credit_limit(const AdaptiveController *c, const SenderSlot *senders, int sid, int active_senders) {
    if (c->config_type == CONFIG_A_FIXED_WINDOW) {
        return c->fixed_window_per_sender;
    }

    uint32_t base_limit = c->current_budget / (uint32_t)(active_senders > 0 ? active_senders : 1);
    if (base_limit < 1) base_limit = 1;

    /* If not DRR or no messages yet received, use uniform budget */
    if (c->config_type != CONFIG_D_ADAPTIVE_DRR || senders[sid].msgs_received == 0) {
        return (base_limit > PER_SENDER_QUEUE_CAP) ? PER_SENDER_QUEUE_CAP : base_limit;
    }

    /* For Config D (DRR): compute byte-equalized credit limit */
    size_t my_frame = senders[sid].bytes_received / senders[sid].msgs_received;
    if (my_frame < sizeof(IpcHeader)) my_frame = sizeof(IpcHeader) + 1024;

    /* Base target byte capacity per sender flow: 64 small packets (~35 KiB) */
    size_t target_bytes = PER_SENDER_QUEUE_CAP * (sizeof(IpcHeader) + 512);
    uint32_t byte_limit = (uint32_t)(target_bytes / my_frame);

    if (byte_limit < 4) byte_limit = 4;
    if (byte_limit > PER_SENDER_QUEUE_CAP) byte_limit = PER_SENDER_QUEUE_CAP;
    return byte_limit;
}

static uint32_t broker_compute_grant(AdaptiveController *c, SenderSlot *senders, int sid, int active_senders) {
    if (!c || sid < 0 || sid >= MAX_SENDERS) return 0;
    if (!senders[sid].active || senders[sid].fd < 0) return 0;

    uint32_t sender_outstanding = tracker_total_outstanding(&senders[sid].tracker);

    if (c->config_type == CONFIG_A_FIXED_WINDOW) {
        return controller_grant_credits_for_sender(c, sid, active_senders, sender_outstanding);
    }

    if (c->config_type != CONFIG_D_ADAPTIVE_DRR) {
        return controller_grant_credits_for_sender(c, sid, active_senders, sender_outstanding);
    }

    /* For Config D (DRR): Withhold credits if outstanding work reaches current budget */
    if (c->outstanding_work >= c->current_budget) {
        return 0;
    }

    uint32_t available_global = c->current_budget - c->outstanding_work;
    uint32_t limit = broker_calc_sender_credit_limit(c, senders, sid, active_senders);
    if (sender_outstanding < limit) {
        uint32_t requested = limit - sender_outstanding;
        uint32_t grant = (requested < available_global) ? requested : available_global;
        if (grant > 0) {
            c->total_granted_credits += grant;
            c->outstanding_work += grant;
            return grant;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    ExperimentConfigType config_type = CONFIG_C_ADAPTIVE_FIFO;
    const char *socket_path = DEFAULT_BROKER_SOCKET;
    int timeout_sec = 0;
    const char *csv_out = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:s:t:o:vh")) != -1) {
        switch (opt) {
            case 'c':
                if (strcmp(optarg, "A") == 0 || strcmp(optarg, "0") == 0) config_type = CONFIG_A_FIXED_WINDOW;
                else if (strcmp(optarg, "B") == 0 || strcmp(optarg, "1") == 0) config_type = CONFIG_B_FIXED_THRESHOLD;
                else if (strcmp(optarg, "C") == 0 || strcmp(optarg, "2") == 0) config_type = CONFIG_C_ADAPTIVE_FIFO;
                else if (strcmp(optarg, "D") == 0 || strcmp(optarg, "3") == 0) config_type = CONFIG_D_ADAPTIVE_DRR;
                break;
            case 's': socket_path = optarg; break;
            case 't': timeout_sec = atoi(optarg); break;
            case 'o': csv_out = optarg; break;
            case 'v': log_set_level(LOG_LEVEL_DEBUG); break;
            case 'h':
            default:
                fprintf(stderr, "Usage: %s [-c A|B|C|D] [-s socket_path] [-t timeout_s] [-o results.csv] [-v]\n", argv[0]);
                return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    LOG_I("Starting IPC Broker [%s]", controller_config_str(config_type));
    LOG_I("Broker listening on: %s", socket_path);

    int listen_fd = ipc_create_listener_socket(socket_path);
    if (listen_fd < 0) {
        LOG_E("Fatal: could not initialize broker socket");
        return 1;
    }

    BrokerQueueManager qm;
    queue_manager_init(&qm);

    AdaptiveController controller;
    controller_init(&controller, config_type);

    BrokerScheduler scheduler;
    scheduler_init(&scheduler, (config_type == CONFIG_D_ADAPTIVE_DRR) ? DISPATCH_DRR : DISPATCH_FIFO);

    SenderSlot senders[MAX_SENDERS];
    memset(senders, 0, sizeof(senders));
    for (int i = 0; i < MAX_SENDERS; i++) senders[i].fd = -1;

    int receiver_fd = -1;

    /* Setup pollfd array:
     * fds[0]: listen_fd
     * fds[1]: receiver_fd (if connected)
     * fds[2 .. 2 + MAX_SENDERS - 1]: sender fds
     */
    #define POLL_LISTEN_IDX   0
    #define POLL_RECEIVER_IDX 1
    #define POLL_SENDER_START 2
    #define TOTAL_POLL_FDS    (POLL_SENDER_START + MAX_SENDERS)

    struct pollfd pfds[TOTAL_POLL_FDS];

    uint64_t start_time_ns = get_time_ns();
    uint64_t last_controller_tick = start_time_ns;

    while (g_running) {
        if (timeout_sec > 0) {
            double elapsed_s = (double)(get_time_ns() - start_time_ns) * 1e-9;
            if (elapsed_s >= timeout_sec) {
                LOG_I("Broker watchdog timeout (%ds) reached. Commencing clean shutdown...", timeout_sec);
                break;
            }
        }

        /* Rebuild poll array */
        memset(pfds, 0, sizeof(pfds));
        pfds[POLL_LISTEN_IDX].fd = listen_fd;
        pfds[POLL_LISTEN_IDX].events = POLLIN;

        if (receiver_fd >= 0) {
            pfds[POLL_RECEIVER_IDX].fd = receiver_fd;
            pfds[POLL_RECEIVER_IDX].events = POLLIN;
        } else {
            pfds[POLL_RECEIVER_IDX].fd = -1;
        }

        int active_sender_count = 0;
        for (int i = 0; i < MAX_SENDERS; i++) {
            int p_idx = POLL_SENDER_START + i;
            if (senders[i].active && senders[i].fd >= 0) {
                pfds[p_idx].fd = senders[i].fd;
                pfds[p_idx].events = POLLIN;
                active_sender_count++;
            } else {
                pfds[p_idx].fd = -1;
            }
        }

        int poll_timeout_ms = CONTROLLER_INTERVAL_MS;
        int ret = poll(pfds, TOTAL_POLL_FDS, poll_timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_E("poll() error: %s", strerror(errno));
            break;
        }

        uint64_t now_ns = get_time_ns();

        /* ── 1. Periodic Controller Tick (Every 50 ms) ──────────────── */
        if ((now_ns - last_controller_tick) >= (CONTROLLER_INTERVAL_MS * 1000000ULL)) {
            controller_tick(&controller, queue_get_utilization(&qm), active_sender_count);
            last_controller_tick = now_ns;

            /* Grant credits to active senders */
            for (int i = 0; i < MAX_SENDERS; i++) {
                if (senders[i].active && senders[i].fd >= 0) {
                    uint32_t grant = broker_compute_grant(&controller, senders, i, active_sender_count);
                    if (grant > 0) {
                        IpcMessage cmsg;
                        memset(&cmsg, 0, sizeof(cmsg));
                        cmsg.header.magic = IPC_MAGIC;
                        cmsg.header.version = IPC_VERSION;
                        cmsg.header.msg_type = MSG_CREDIT;
                        cmsg.header.sender_id = i;
                        cmsg.header.credit_val = grant;
                        cmsg.header.timestamp_ns = now_ns;
                        cmsg.header.payload_len = 0;
                        cmsg.header.checksum = 0;

                        if (ipc_send_msg(senders[i].fd, &cmsg) > 0) {
                            tracker_on_credit_granted(&senders[i].tracker, grant);
                            LOG_D("Granted %u credits to Sender %d (held=%u, inflight=%u, budget=%u)",
                                  grant, i, senders[i].tracker.credits_held,
                                  senders[i].tracker.messages_in_flight,
                                  controller.current_budget);
                        }
                    }
                }
            }
        }

        /* ── 2. Accept New Inbound Connections ──────────────────────── */
        if (pfds[POLL_LISTEN_IDX].revents & POLLIN) {
            int new_fd = accept(listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                IpcMessage reg_msg;
                int r = ipc_recv_msg(new_fd, &reg_msg);
                if (r > 0 && reg_msg.header.msg_type == MSG_REGISTER) {
                    if (reg_msg.header.sender_id == -1) {
                        /* Role: RECEIVER */
                        if (receiver_fd >= 0) close(receiver_fd);
                        receiver_fd = new_fd;
                        LOG_I("Receiver process successfully registered on fd %d", receiver_fd);
                    } else {
                        /* Role: SENDER */
                        int sid = reg_msg.header.sender_id;
                        if (sid >= 0 && sid < MAX_SENDERS) {
                            if (senders[sid].fd >= 0) close(senders[sid].fd);
                            senders[sid].fd = new_fd;
                            senders[sid].active = true;
                            tracker_init(&senders[sid].tracker);
                            ipc_set_nonblocking(senders[sid].fd, true);
                            LOG_I("Sender %d successfully registered on fd %d", sid, new_fd);

                            uint32_t init_credits = (config_type == CONFIG_D_ADAPTIVE_DRR) ? 8 : 16;
                            IpcMessage cmsg;
                            memset(&cmsg, 0, sizeof(cmsg));
                            cmsg.header.magic = IPC_MAGIC;
                            cmsg.header.version = IPC_VERSION;
                            cmsg.header.msg_type = MSG_CREDIT;
                            cmsg.header.sender_id = sid;
                            cmsg.header.credit_val = init_credits;
                            cmsg.header.timestamp_ns = now_ns;

                            if (ipc_send_msg(new_fd, &cmsg) > 0) {
                                tracker_on_credit_granted(&senders[sid].tracker, init_credits);
                                controller.total_granted_credits += init_credits;
                                controller.outstanding_work += init_credits;
                            }
                        } else {
                            close(new_fd);
                        }
                    }
                } else {
                    close(new_fd);
                }
            }
        }

        /* ── 3. Handle Receiver Activity (ACKs & Disconnect) ────────── */
        if (receiver_fd >= 0 && (pfds[POLL_RECEIVER_IDX].revents & (POLLIN | POLLHUP | POLLERR))) {
            IpcMessage ack_msg;
            int r;
            while ((r = recv(receiver_fd, &ack_msg, sizeof(IpcMessage), MSG_DONTWAIT)) > 0) {
                if ((size_t)r >= sizeof(IpcHeader) && ipc_msg_validate(&ack_msg)) {
                    if (ack_msg.header.msg_type == MSG_ACK) {
                        int sid = ack_msg.header.sender_id;
                        uint64_t aseq = ack_msg.header.msg_seq;
                        if (sid >= 0 && sid < MAX_SENDERS) {
                            /* Validate ACK against active in-flight sequence numbers */
                            if (!tracker_on_ack_received(&senders[sid].tracker, aseq)) {
                                LOG_W("Dropped invalid/duplicate/stale ACK from Sender %d (seq=%lu)",
                                      sid, (unsigned long)aseq);
                            } else {
                                controller_on_message_completion(&controller, sid);

                                /* Prompt credit replenishment if sender is still active and budget allows */
                                if (senders[sid].active && senders[sid].fd >= 0) {
                                    uint32_t grant = broker_compute_grant(&controller, senders, sid,
                                                                          active_sender_count > 0 ? active_sender_count : 1);
                                    if (grant > 0) {
                                        IpcMessage cmsg;
                                        memset(&cmsg, 0, sizeof(cmsg));
                                        cmsg.header.magic = IPC_MAGIC;
                                        cmsg.header.version = IPC_VERSION;
                                        cmsg.header.msg_type = MSG_CREDIT;
                                        cmsg.header.sender_id = sid;
                                        cmsg.header.credit_val = grant;
                                        cmsg.header.timestamp_ns = now_ns;
                                        if (ipc_send_msg(senders[sid].fd, &cmsg) > 0) {
                                            tracker_on_credit_granted(&senders[sid].tracker, grant);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (r == 0) {
                LOG_W("Receiver disconnected cleanly. Awaiting reconnect...");
                close(receiver_fd);
                receiver_fd = -1;
            } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_W("Receiver disconnected (errno=%d). Awaiting reconnect...", errno);
                close(receiver_fd);
                receiver_fd = -1;
            }
        }

        /* ── 4. Handle Sender Activity (DATA, STOP & Disconnect) ─────── */
        for (int i = 0; i < MAX_SENDERS; i++) {
            int p_idx = POLL_SENDER_START + i;
            if (!senders[i].active || senders[i].fd < 0) continue;

            if (pfds[p_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                while (senders[i].active && senders[i].fd >= 0 &&
                       qm.queues[i].count < PER_SENDER_QUEUE_CAP &&
                       qm.global_count < GLOBAL_QUEUE_CAP) {
                    IpcMessage msg;
                    int r = ipc_recv_msg(senders[i].fd, &msg);
                    if (r > 0) {
                        if (msg.header.msg_type == MSG_DATA) {
                            QueueResult qres = queue_enqueue(&qm, i, &msg);
                            if (qres == QUEUE_OK) {
                                tracker_on_data_sent(&senders[i].tracker, msg.header.msg_seq);
                                controller_on_message_arrival(&controller, i, msg.header.payload_len);
                                senders[i].msgs_received++;
                                senders[i].bytes_received += (sizeof(IpcHeader) + msg.header.payload_len);
                            } else {
                                LOG_W("Dropped DATA from Sender %d (QueueResult=%d, GlobalCount=%zu, GlobalBytes=%zu)",
                                      i, qres, qm.global_count, qm.global_queued_bytes);
                            }
                        } else if (msg.header.msg_type == MSG_STOP) {
                            LOG_I("Sender %d reported STOP", i);
                            close(senders[i].fd);
                            senders[i].fd = -1;
                            senders[i].active = false;
                            /* Reclaim unused credits held by sender */
                            uint32_t unused = tracker_reclaim_unused(&senders[i].tracker);
                            if (unused > 0) {
                                controller_reclaim_credits(&controller, unused);
                                LOG_I("Reclaimed %u unused credits from Sender %d on STOP (remaining outstanding_work=%u)",
                                      unused, i, controller.outstanding_work);
                            }
                            break;
                        }
                    } else if (r == -EAGAIN || r == -EWOULDBLOCK) {
                        break;
                    } else if (r == 0) {
                        LOG_I("Sender %d disconnected cleanly", i);
                        close(senders[i].fd);
                        senders[i].fd = -1;
                        senders[i].active = false;
                        uint32_t unused = tracker_reclaim_unused(&senders[i].tracker);
                        if (unused > 0) {
                            controller_reclaim_credits(&controller, unused);
                            LOG_I("Reclaimed %u unused credits from Sender %d on disconnect (remaining outstanding_work=%u)",
                                  unused, i, controller.outstanding_work);
                        }
                        break;
                    } else {
                        LOG_W("Sender %d socket error (%d). Closing.", i, r);
                        close(senders[i].fd);
                        senders[i].fd = -1;
                        senders[i].active = false;
                        uint32_t unused = tracker_reclaim_unused(&senders[i].tracker);
                        if (unused > 0) {
                            controller_reclaim_credits(&controller, unused);
                            LOG_I("Reclaimed %u unused credits from Sender %d on error (remaining outstanding_work=%u)",
                                  unused, i, controller.outstanding_work);
                        }
                        break;
                    }
                }
            }
        }

        /* ── 5. Dispatch Queued Work to Receiver ────────────────────── */
        if (receiver_fd >= 0 && qm.global_count > 0) {
            IpcMessage disp_msg;
            int chosen_sender = -1;
            /* Dispatch while messages are pending and receiver is ready */
            while (qm.global_count > 0) {
                QueueResult dres = scheduler_dispatch_next(&scheduler, &qm, &disp_msg, &chosen_sender);
                if (dres == QUEUE_OK) {
                    int sent = ipc_send_msg(receiver_fd, &disp_msg);
                    if (sent <= 0) {
                        LOG_W("Failed to forward message to receiver (sent=%d)", sent);
                        break;
                    }
                } else {
                    break;
                }
            }
        }
    }

    /* ── Teardown & Reporting ───────────────────────────────────────── */
    LOG_I("Broker shutting down. Closing sockets...");

    /* Notify receiver of shutdown */
    if (receiver_fd >= 0) {
        IpcMessage stop_msg;
        memset(&stop_msg, 0, sizeof(stop_msg));
        stop_msg.header.magic = IPC_MAGIC;
        stop_msg.header.version = IPC_VERSION;
        stop_msg.header.msg_type = MSG_STOP;
        ipc_send_msg(receiver_fd, &stop_msg);
        close(receiver_fd);
    }

    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders[i].fd >= 0) {
            IpcMessage stop_msg;
            memset(&stop_msg, 0, sizeof(stop_msg));
            stop_msg.header.magic = IPC_MAGIC;
            stop_msg.header.version = IPC_VERSION;
            stop_msg.header.msg_type = MSG_STOP;
            ipc_send_msg(senders[i].fd, &stop_msg);
            close(senders[i].fd);
        }
    }

    close(listen_fd);
    ipc_unlink_socket(socket_path);

    double total_runtime_s = (double)(get_time_ns() - start_time_ns) * 1e-9;
    double throughput_msgs = (total_runtime_s > 0) ? (double)scheduler.total_dispatched_msgs / total_runtime_s : 0;
    double throughput_mb = (total_runtime_s > 0) ? ((double)scheduler.total_dispatched_bytes / (1024.0 * 1024.0)) / total_runtime_s : 0;
    double avg_ctrl_overhead = (controller.tick_count > 0) ? (controller.total_overhead_us / (double)controller.tick_count) : 0;

    int active_s = 0;
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders[i].msgs_received > 0 || scheduler.dispatched_msgs[i] > 0) active_s++;
    }
    double byte_fairness = scheduler_get_byte_fairness(&scheduler, active_s > 0 ? active_s : 1);

    printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    BROKER RUNTIME SUMMARY                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  Configuration     : %-43s ║\n", controller_config_str(config_type));
    printf("║  Elapsed Runtime   : %8.3f seconds                               ║\n", total_runtime_s);
    printf("║  Total Admitted    : %-8zu messages                              ║\n", qm.total_enqueued);
    printf("║  Total Dispatched  : %-8lu messages                              ║\n", (unsigned long)scheduler.total_dispatched_msgs);
    printf("║  Total Dropped     : %-8zu messages                              ║\n", qm.total_dropped);
    printf("║  Peak Queue Depth  : %-8zu / %-4d messages                       ║\n", qm.peak_count, GLOBAL_QUEUE_CAP);
    printf("║  Peak Queued Memory: %8.2f KiB / %-4d KiB                        ║\n", (double)qm.peak_bytes / 1024.0, MAX_QUEUED_BYTES / 1024);
    printf("║  Throughput        : %8.2f msgs/s  (%6.2f MB/s)                 ║\n", throughput_msgs, throughput_mb);
    printf("║  Jain Byte Fairness: %8.4f                                       ║\n", byte_fairness);
    printf("║  Ctrl State Changes: %-8u                                       ║\n", controller.state_transitions);
    printf("║  Avg Ctrl Overhead : %8.3f µs/tick                               ║\n", avg_ctrl_overhead);
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    if (csv_out) {
        FILE *cf = fopen(csv_out, "w");
        if (cf) {
            const char *cfg_tag = "Config_D";
            if (config_type == CONFIG_A_FIXED_WINDOW) cfg_tag = "Config_A";
            else if (config_type == CONFIG_B_FIXED_THRESHOLD) cfg_tag = "Config_B";
            else if (config_type == CONFIG_C_ADAPTIVE_FIFO) cfg_tag = "Config_C";

            fprintf(cf, "config,runtime_s,admitted,dispatched,dropped,peak_queue,peak_bytes,throughput_msgs,throughput_mb,byte_fairness,state_changes,ctrl_overhead_us\n");
            fprintf(cf, "%s,%.3f,%zu,%lu,%zu,%zu,%zu,%.2f,%.2f,%.4f,%u,%.3f\n",
                    cfg_tag, total_runtime_s, qm.total_enqueued,
                    (unsigned long)scheduler.total_dispatched_msgs, qm.total_dropped,
                    qm.peak_count, qm.peak_bytes, throughput_msgs, throughput_mb,
                    byte_fairness, controller.state_transitions, avg_ctrl_overhead);
            fclose(cf);
            LOG_I("Summary metrics written to '%s'", csv_out);
        }
    }

    queue_manager_cleanup(&qm);
    return 0;
}
