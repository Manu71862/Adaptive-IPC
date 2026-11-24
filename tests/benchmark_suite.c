/**
 * benchmark_suite.c — Comprehensive Automated Evaluation Harness
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Evaluates the 4 Experimental Configurations:
 *   Config A: Fixed-window baseline, FIFO dispatch
 *   Config B: Fixed-threshold backpressure, FIFO dispatch
 *   Config C: Adaptive backpressure, FIFO dispatch
 *   Config D: Adaptive backpressure, Deficit Round-Robin (DRR) dispatch
 *
 * Across the 6 Benchmark Scenarios:
 *   1. Normal Traffic (symmetric steady load)
 *   2. Sudden Bursts (bursty traffic with idle gaps)
 *   3. Sustained Overload (high-rate ingress exceeding receiver capacity)
 *   4. Fairness Exp A (Equal Offered Bytes: 400x512B vs 50x4096B)
 *   5. Fairness Exp B (Continuous Backlog: 500x512B vs 100x4096B, 200 msgs evaluated)
 *   6. Receiver Slowdown & Recovery (induced 250ms receiver stall)
 *
 * Strictly C/C++. No Python.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>

#include "ipc_protocol.h"
#include "common.h"

#define BENCH_SOCKET "/tmp/ipc_bench_runner.sock"

typedef struct {
    const char *config_code; /* "A", "B", "C", "D" */
    const char *config_name;
    const char *scenario_name;
    double      runtime_s;
    uint64_t    msgs_received;
    double      throughput_mps;
    double      throughput_mb;
    double      p50_lat_us;
    double      p95_lat_us;
    double      p99_lat_us;
    double      mean_lat_us;
    size_t      peak_queue_depth;
    size_t      peak_queue_bytes;
    double      jain_fairness;
    uint32_t    state_transitions;
    double      ctrl_overhead_us;
    size_t      packet_drops;
    uint64_t    sender_msgs[MAX_SENDERS];
    uint64_t    sender_bytes[MAX_SENDERS];
    int         num_flows;
} ScenarioResult;

static bool run_scenario(const char *cfg_code, const char *scenario, int scenario_id, ScenarioResult *res) {
    memset(res, 0, sizeof(ScenarioResult));
    res->config_code = cfg_code;
    res->scenario_name = scenario;

    if (strcmp(cfg_code, "A") == 0) res->config_name = "Config A (Fixed Window, FIFO)";
    else if (strcmp(cfg_code, "B") == 0) res->config_name = "Config B (Fixed Threshold, FIFO)";
    else if (strcmp(cfg_code, "C") == 0) res->config_name = "Config C (Adaptive EWMA, FIFO)";
    else res->config_name = "Config D (Adaptive EWMA, DRR)";

    ipc_unlink_socket(BENCH_SOCKET);

    char broker_csv[128];
    char recv_csv[128];
    snprintf(broker_csv, sizeof(broker_csv), "/tmp/bench_broker_%s_%d.csv", cfg_code, scenario_id);
    snprintf(recv_csv, sizeof(recv_csv), "/tmp/bench_recv_%s_%d.csv", cfg_code, scenario_id);
    unlink(broker_csv);
    unlink(recv_csv);

    /* 1. Launch Broker */
    pid_t broker_pid = fork();
    if (broker_pid == 0) {
        FILE *f = freopen("/dev/null", "w", stderr); (void)f;
        execl("./bin/broker", "broker", "-c", cfg_code, "-s", BENCH_SOCKET,
              "-t", "10", "-o", broker_csv, NULL);
        _exit(127);
    }
    sleep_ms(150); /* Allow broker listener to bind */

    /* 2. Configure & Launch Receiver */
    int total_expected = 600;
    int stall_after = 0;
    int stall_dur_ms = 0;
    int proc_delay_us = 50; /* 50 us base processing */

    if (scenario_id == 4) {
        /* Scenario 4: Fairness Exp A (Equal Offered Bytes: 400x512B vs 50x4096B) */
        total_expected = 450;
    } else if (scenario_id == 5) {
        /* Scenario 5: Fairness Exp B (Continuous Backlog: evaluated over 200 concurrent msgs) */
        total_expected = 200;
    } else if (scenario_id == 6) {
        /* Scenario 6: Receiver Slowdown and Recovery */
        stall_after = 200;
        stall_dur_ms = 250;
    }

    char exp_msgs_str[16], proc_delay_str[16], stall_after_str[16], stall_dur_str[16];
    snprintf(exp_msgs_str, sizeof(exp_msgs_str), "%d", total_expected);
    snprintf(proc_delay_str, sizeof(proc_delay_str), "%d", proc_delay_us);
    snprintf(stall_after_str, sizeof(stall_after_str), "%d", stall_after);
    snprintf(stall_dur_str, sizeof(stall_dur_str), "%d", stall_dur_ms);

    /* Explicit participant count for all scenarios (Scenarios 4 & 5: 2 flows; Scenarios 1, 2, 3, 6: 3 flows) */
    int flow_count = (scenario_id == 4 || scenario_id == 5) ? 2 : 3;
    char flow_count_str[8];
    snprintf(flow_count_str, sizeof(flow_count_str), "%d", flow_count);

    pid_t receiver_pid = fork();
    if (receiver_pid == 0) {
        FILE *f = freopen("/dev/null", "w", stderr); (void)f;
        execl("./bin/receiver", "receiver", "-k", BENCH_SOCKET,
              "-n", exp_msgs_str, "-d", proc_delay_str,
              "-s", stall_after_str, "-t", stall_dur_str,
              "-o", recv_csv, "-f", flow_count_str, NULL);
        _exit(127);
    }
    sleep_ms(100);

    /* 3. Launch Senders based on Scenario */
    pid_t sender_pids[MAX_SENDERS];
    int num_senders = 0;

    if (scenario_id == 4) {
        /* Scenario 4: Fairness Exp A (Equal Offered Bytes: 400x512B vs 50x4096B) */
        num_senders = 2;

        /* Sender 0: 400 msgs * 512B */
        sender_pids[0] = fork();
        if (sender_pids[0] == 0) {
            FILE *f = freopen("/dev/null", "w", stderr); (void)f;
            execl("./bin/sender", "sender", "-k", BENCH_SOCKET, "-i", "0",
                  "-n", "400", "-s", "512", NULL);
            _exit(127);
        }

        sleep_ms(20);

        /* Sender 1: 50 msgs * 4096B */
        sender_pids[1] = fork();
        if (sender_pids[1] == 0) {
            FILE *f = freopen("/dev/null", "w", stderr); (void)f;
            execl("./bin/sender", "sender", "-k", BENCH_SOCKET, "-i", "1",
                  "-n", "50", "-s", "4096", NULL);
            _exit(127);
        }
    } else if (scenario_id == 5) {
        /*
         * Scenario 5: Fairness Exp B (Continuous Backlog)
         * Deterministic Two-Way Rendezvous Barrier using POSIX pipes:
         * 1. Sender 0 offers 500 msgs * 512B; Sender 1 offers 250 msgs * 4096B.
         *    Both offerings are sufficiently large that neither flow exhausts during
         *    the 200-message measurement window under either FIFO or DRR.
         * 2. Both senders connect, register, receive initial credits, and write 1 byte
         *    to ready_pipe[1] to signal readiness before blocking on sync_pipe[0].
         * 3. Harness reads 2 bytes from ready_pipe[0], ensuring both senders are armed,
         *    then closes sync_pipe[1] to release both senders simultaneously at the exact
         *    same instant without any arbitrary sleep.
         */
        num_senders = 2;
        int ready_pipe[2];
        int sync_pipe[2];
        if (pipe(ready_pipe) < 0 || pipe(sync_pipe) < 0) {
            perror("pipe");
        }
        char ready_fd_str[16];
        char sync_fd_str[16];
        snprintf(ready_fd_str, sizeof(ready_fd_str), "%d", ready_pipe[1]);
        snprintf(sync_fd_str, sizeof(sync_fd_str), "%d", sync_pipe[0]);

        /* Sender 0: 500 msgs * 512B */
        sender_pids[0] = fork();
        if (sender_pids[0] == 0) {
            close(ready_pipe[0]);
            close(sync_pipe[1]);
            FILE *f = freopen("/dev/null", "w", stderr); (void)f;
            execl("./bin/sender", "sender", "-k", BENCH_SOCKET, "-i", "0",
                  "-n", "500", "-s", "512", "-R", ready_fd_str, "-w", sync_fd_str, NULL);
            _exit(127);
        }

        /* Sender 1: 250 msgs * 4096B */
        sender_pids[1] = fork();
        if (sender_pids[1] == 0) {
            close(ready_pipe[0]);
            close(sync_pipe[1]);
            FILE *f = freopen("/dev/null", "w", stderr); (void)f;
            execl("./bin/sender", "sender", "-k", BENCH_SOCKET, "-i", "1",
                  "-n", "250", "-s", "4096", "-R", ready_fd_str, "-w", sync_fd_str, NULL);
            _exit(127);
        }

        /* Harness closes sender ends of pipes */
        close(ready_pipe[1]);
        close(sync_pipe[0]);

        /* Wait until both senders have connected, registered, and armed with credits */
        char r_buf[2];
        size_t ready_count = 0;
        struct pollfd pfd_ready;
        pfd_ready.fd = ready_pipe[0];
        pfd_ready.events = POLLIN;
        while (ready_count < 2) {
            int pret = poll(&pfd_ready, 1, 5000);
            if (pret <= 0) {
                fprintf(stderr, "Warning: timeout waiting for sender readiness\n");
                break;
            }
            ssize_t n = read(ready_pipe[0], r_buf + ready_count, 2 - ready_count);
            if (n <= 0) break;
            ready_count += (size_t)n;
        }
        close(ready_pipe[0]);

        /* Release barrier simultaneously (0 arbitrary sleep) */
        close(sync_pipe[1]);
    } else {
        /* Scenarios 1, 2, 3, 6: 3 Senders */
        num_senders = 3;
        int msgs_per_sender = total_expected / num_senders;
        char num_str[16];
        snprintf(num_str, sizeof(num_str), "%d", msgs_per_sender);

        int burst_sz = 1;
        int burst_delay = 0;
        int rate_limit = 0;
        size_t p_size = 1024;

        if (scenario_id == 2) {
            /* Scenario 2: Sudden Bursts */
            burst_sz = 20;
            burst_delay = 50;
        } else if (scenario_id == 3) {
            /* Scenario 3: Sustained Overload (unthrottled large payload) */
            p_size = 2048;
        }

        char burst_sz_str[16], burst_del_str[16], rate_str[16], psz_str[16];
        snprintf(burst_sz_str, sizeof(burst_sz_str), "%d", burst_sz);
        snprintf(burst_del_str, sizeof(burst_del_str), "%d", burst_delay);
        snprintf(rate_str, sizeof(rate_str), "%d", rate_limit);
        snprintf(psz_str, sizeof(psz_str), "%zu", p_size);

        for (int i = 0; i < num_senders; i++) {
            char id_str[16];
            snprintf(id_str, sizeof(id_str), "%d", i);

            sender_pids[i] = fork();
            if (sender_pids[i] == 0) {
                FILE *f = freopen("/dev/null", "w", stderr); (void)f;
                execl("./bin/sender", "sender", "-k", BENCH_SOCKET, "-i", id_str,
                      "-n", num_str, "-s", psz_str, "-b", burst_sz_str,
                      "-d", burst_del_str, "-r", rate_str, NULL);
                _exit(127);
            }
        }
    }

    /* Wait for Receiver to complete the target workload */
    int r_status = 0;
    waitpid(receiver_pid, &r_status, 0);

    /* Signal Broker to terminate and dump metrics */
    kill(broker_pid, SIGINT);
    waitpid(broker_pid, NULL, 0);

    /* Clean up Senders if still running */
    for (int i = 0; i < num_senders; i++) {
        kill(sender_pids[i], SIGINT);
        waitpid(sender_pids[i], NULL, 0);
    }

    ipc_unlink_socket(BENCH_SOCKET);

    /* Parse receiver CSV */
    FILE *rf = fopen(recv_csv, "r");
    if (rf) {
        char line[1024];
        if (fgets(line, sizeof(line), rf) && fgets(line, sizeof(line), rf)) {
            unsigned long r_msgs = 0, r_vol = 0;
            double r_time = 0, r_tput_m = 0, r_tput_mb = 0, r_p50 = 0, r_p95 = 0, r_p99 = 0, r_mean = 0, r_fair = 0;
            unsigned long r_err = 0;
            int r_nflows = 0;
            int n_consumed = 0;
            /* Header format: ...jain_fairness,n_flows[,s0_msgs,s0_bytes,...] */
            if (sscanf(line, "%lu,%lu,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lu,%lf,%d%n",
                       &r_msgs, &r_vol, &r_time, &r_tput_m, &r_tput_mb,
                       &r_p50, &r_p95, &r_p99, &r_mean, &r_err, &r_fair, &r_nflows, &n_consumed) >= 11) {
                res->msgs_received = r_msgs;
                res->runtime_s = r_time;
                res->throughput_mps = r_tput_m;
                res->throughput_mb = r_tput_mb;
                res->p50_lat_us = r_p50;
                res->p95_lat_us = r_p95;
                res->p99_lat_us = r_p99;
                res->mean_lat_us = r_mean;
                res->jain_fairness = r_fair;
                res->num_flows = r_nflows;

                if (n_consumed > 0) {
                    char *ptr = line + n_consumed;
                    for (int i = 0; i < r_nflows && i < MAX_SENDERS; i++) {
                        unsigned long sm = 0, sb = 0;
                        int c = 0;
                        if (sscanf(ptr, ",%lu,%lu%n", &sm, &sb, &c) >= 2) {
                            res->sender_msgs[i] = sm;
                            res->sender_bytes[i] = sb;
                            ptr += c;
                        }
                    }
                }
            }
        }
        fclose(rf);
        unlink(recv_csv);
    }

    /* Parse broker CSV */
    FILE *bf = fopen(broker_csv, "r");
    if (bf) {
        char line[512];
        if (fgets(line, sizeof(line), bf) && fgets(line, sizeof(line), bf)) {
            char cfg_str[64];
            double b_time = 0, b_tput = 0, b_mb = 0, b_fair = 0, b_ovh = 0;
            size_t b_adm = 0, b_disp = 0, b_drop = 0, b_peak_q = 0, b_peak_b = 0;
            uint32_t b_state_chg = 0;
            if (sscanf(line, "%63[^,],%lf,%zu,%zu,%zu,%zu,%zu,%lf,%lf,%lf,%u,%lf",
                       cfg_str, &b_time, &b_adm, &b_disp, &b_drop,
                       &b_peak_q, &b_peak_b, &b_tput, &b_mb,
                       &b_fair, &b_state_chg, &b_ovh) >= 11) {
                res->peak_queue_depth = b_peak_q;
                res->peak_queue_bytes = b_peak_b;
                res->packet_drops = b_drop;
                res->state_transitions = b_state_chg;
                res->ctrl_overhead_us = b_ovh;
            }
        }
        fclose(bf);
        unlink(broker_csv);
    }

    return true;
}

int main(int argc, char *argv[]) {
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║      ADAPTIVE IPC BACKPRESSURE & FAIRNESS BENCHMARK HARNESS      ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    mkdir("results", 0755);

    const char *configs[] = { "A", "B", "C", "D" };
    const char *scenarios[] = {
        "1. Normal Traffic",
        "2. Sudden Bursts",
        "3. Sustained Overload",
        "4. Fairness Exp A (Equal Offered Bytes)",
        "5. Fairness Exp B (Continuous Backlog)",
        "6. Receiver Slowdown & Recovery"
    };
    int num_configs = 4;
    int num_scenarios = 6;

    ScenarioResult results[30];
    int res_idx = 0;

    for (int s = 0; s < num_scenarios; s++) {
        printf("────────────────────────────────────────────────────────────────────\n");
        printf("Executing Scenario %d: %s\n", s + 1, scenarios[s]);
        printf("────────────────────────────────────────────────────────────────────\n");

        for (int c = 0; c < num_configs; c++) {
            printf("  Running %s with Config %s...", scenarios[s], configs[c]);
            fflush(stdout);

            ScenarioResult r;
            run_scenario(configs[c], scenarios[s], s + 1, &r);
            results[res_idx++] = r;

            printf(" DONE (Tput: %.1f msgs/s, p50: %.1f µs, p99: %.1f µs, PeakQ: %zu, Fair: %.4f)\n",
                   r.throughput_mps, r.p50_lat_us, r.p99_lat_us, r.peak_queue_depth, r.jain_fairness);
        }
        printf("\n");
    }

    /* ── Write Comprehensive Summary CSV ────────────────────────────── */
    const char *summary_csv = (argc > 1) ? argv[1] : "results/benchmark_summary.csv";
    FILE *f_csv = fopen(summary_csv, "w");
    if (f_csv) {
        fprintf(f_csv, "scenario,config,throughput_msgs_per_s,throughput_mib_per_s,p50_us,p95_us,p99_us,mean_us,peak_queue_msgs,peak_memory_kib,jain_fairness,packet_drops,state_changes,ctrl_overhead_us,s0_msgs,s0_bytes,s1_msgs,s1_bytes,s2_msgs,s2_bytes\n");
        for (int i = 0; i < res_idx; i++) {
            fprintf(f_csv, "\"%s\",\"%s\",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%zu,%.2f,%.4f,%zu,%u,%.3f,%lu,%lu,%lu,%lu,%lu,%lu\n",
                    results[i].scenario_name, results[i].config_name,
                    results[i].throughput_mps, results[i].throughput_mb,
                    results[i].p50_lat_us, results[i].p95_lat_us, results[i].p99_lat_us, results[i].mean_lat_us,
                    results[i].peak_queue_depth, (double)results[i].peak_queue_bytes / 1024.0,
                    results[i].jain_fairness, results[i].packet_drops,
                    results[i].state_transitions, results[i].ctrl_overhead_us,
                    (unsigned long)results[i].sender_msgs[0], (unsigned long)results[i].sender_bytes[0],
                    (unsigned long)results[i].sender_msgs[1], (unsigned long)results[i].sender_bytes[1],
                    (unsigned long)results[i].sender_msgs[2], (unsigned long)results[i].sender_bytes[2]);
        }
        fclose(f_csv);
        printf("Full benchmark summary saved to: %s\n\n", summary_csv);
    }

    /* ── Print Summary Comparative Table ────────────────────────────── */
    printf("╔═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                                                  EXPERIMENTAL EVALUATION SUMMARY RESULTS                                                    ║\n");
    printf("╠═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-38s │ %-6s │ %-12s │ %-9s │ %-9s │ %-5s │ %-8s │ %-24s ║\n",
           "Scenario", "Config", "Throughput", "p50 (µs)", "p99 (µs)", "PeakQ", "Fairness", "Delivered (s0/s1/s2 msgs)");
    printf("╟────────────────────────────────────────┼────────┼──────────────┼───────────┼───────────┼───────┼──────────┼──────────────────────────╢\n");

    for (int i = 0; i < res_idx; i++) {
        char flow_summary[32];
        if (results[i].num_flows == 2) {
            snprintf(flow_summary, sizeof(flow_summary), "%lu / %lu",
                     (unsigned long)results[i].sender_msgs[0], (unsigned long)results[i].sender_msgs[1]);
        } else {
            snprintf(flow_summary, sizeof(flow_summary), "%lu / %lu / %lu",
                     (unsigned long)results[i].sender_msgs[0], (unsigned long)results[i].sender_msgs[1], (unsigned long)results[i].sender_msgs[2]);
        }
        printf("║ %-38s │ %-6s │ %8.1f ms/s │ %9.1f │ %9.1f │ %5zu │ %8.4f │ %-24s ║\n",
               results[i].scenario_name, results[i].config_code,
               results[i].throughput_mps, results[i].p50_lat_us, results[i].p99_lat_us,
               results[i].peak_queue_depth, results[i].jain_fairness, flow_summary);
    }
    printf("╚═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝\n\n");

    return 0;
}
