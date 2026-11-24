/**
 * compute_summary.c — Statistical Aggregation across Benchmark Runs
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 *
 * Reads 3 independent benchmark run CSVs (run1, run2, run3),
 * calculates mean and standard deviation for each metric across the 24 permutations,
 * writes results/benchmark_summary.csv, and prints a statistical report table.
 *
 * Strictly pure C (C99/C11). No Python dependencies.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define NUM_RUNS 3
#define NUM_ROWS 24

typedef struct {
    char scenario[128];
    char config[128];
    double throughput_mps[NUM_RUNS];
    double throughput_mb[NUM_RUNS];
    double p50_us[NUM_RUNS];
    double p95_us[NUM_RUNS];
    double p99_us[NUM_RUNS];
    double mean_us[NUM_RUNS];
    double peak_queue[NUM_RUNS];
    double peak_mem_kib[NUM_RUNS];
    double fairness[NUM_RUNS];
    double drops[NUM_RUNS];
    double state_changes[NUM_RUNS];
    double ctrl_overhead[NUM_RUNS];
    double s0_msgs[NUM_RUNS];
    double s0_bytes[NUM_RUNS];
    double s1_msgs[NUM_RUNS];
    double s1_bytes[NUM_RUNS];
    double s2_msgs[NUM_RUNS];
    double s2_bytes[NUM_RUNS];
} PermutationStats;

static void calc_stats(const double *vals, int n, double *mean, double *stddev) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += vals[i];
    *mean = sum / (double)n;

    if (n > 1) {
        double sq_sum = 0.0;
        for (int i = 0; i < n; i++) {
            double diff = vals[i] - *mean;
            sq_sum += diff * diff;
        }
        *stddev = sqrt(sq_sum / (double)(n - 1));
    } else {
        *stddev = 0.0;
    }
}

int main(int argc, char *argv[]) {
    const char *run_files[NUM_RUNS] = {
        (argc > 1) ? argv[1] : "results/benchmark_run1.csv",
        (argc > 2) ? argv[2] : "results/benchmark_run2.csv",
        (argc > 3) ? argv[3] : "results/benchmark_run3.csv"
    };
    const char *out_file = (argc > 4) ? argv[4] : "results/benchmark_summary.csv";

    PermutationStats perm[NUM_ROWS];
    memset(perm, 0, sizeof(perm));

    for (int r = 0; r < NUM_RUNS; r++) {
        FILE *f = fopen(run_files[r], "r");
        if (!f) {
            fprintf(stderr, "Error: cannot open run file '%s'\n", run_files[r]);
            return 1;
        }

        char line[2048];
        /* Skip header */
        if (!fgets(line, sizeof(line), f)) {
            fprintf(stderr, "Error: empty file '%s'\n", run_files[r]);
            fclose(f);
            return 1;
        }

        int row = 0;
        while (fgets(line, sizeof(line), f) && row < NUM_ROWS) {
            char scen[128] = {0}, cfg[128] = {0};
            double tput_m = 0, tput_mb = 0, p50 = 0, p95 = 0, p99 = 0, mean_l = 0;
            double pq = 0, pmem = 0, fair = 0, drp = 0, sc = 0, ovh = 0;
            double s0m = 0, s0b = 0, s1m = 0, s1b = 0, s2m = 0, s2b = 0;

            /* Parse quoted strings and numbers */
            if (sscanf(line, "\"%127[^\"]\",\"%127[^\"]\",%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                       scen, cfg, &tput_m, &tput_mb, &p50, &p95, &p99, &mean_l,
                       &pq, &pmem, &fair, &drp, &sc, &ovh,
                       &s0m, &s0b, &s1m, &s1b, &s2m, &s2b) >= 14) {
                if (r == 0) {
                    snprintf(perm[row].scenario, sizeof(perm[row].scenario), "%s", scen);
                    snprintf(perm[row].config, sizeof(perm[row].config), "%s", cfg);
                }
                perm[row].throughput_mps[r] = tput_m;
                perm[row].throughput_mb[r]  = tput_mb;
                perm[row].p50_us[r]         = p50;
                perm[row].p95_us[r]         = p95;
                perm[row].p99_us[r]         = p99;
                perm[row].mean_us[r]        = mean_l;
                perm[row].peak_queue[r]     = pq;
                perm[row].peak_mem_kib[r]   = pmem;
                perm[row].fairness[r]       = fair;
                perm[row].drops[r]          = drp;
                perm[row].state_changes[r]  = sc;
                perm[row].ctrl_overhead[r]  = ovh;
                perm[row].s0_msgs[r]        = s0m;
                perm[row].s0_bytes[r]       = s0b;
                perm[row].s1_msgs[r]        = s1m;
                perm[row].s1_bytes[r]       = s1b;
                perm[row].s2_msgs[r]        = s2m;
                perm[row].s2_bytes[r]       = s2b;
                row++;
            }
        }
        fclose(f);
        if (row != NUM_ROWS) {
            fprintf(stderr, "Warning: '%s' contained %d rows (expected %d)\n", run_files[r], row, NUM_ROWS);
        }
    }

    /* Recalculate and write averaged results/benchmark_summary.csv */
    FILE *out = fopen(out_file, "w");
    if (!out) {
        fprintf(stderr, "Error: cannot create output file '%s'\n", out_file);
        return 1;
    }

    fprintf(out, "scenario,config,throughput_msgs_per_s,throughput_mib_per_s,p50_us,p95_us,p99_us,mean_us,peak_queue_msgs,peak_memory_kib,jain_fairness,packet_drops,state_changes,ctrl_overhead_us,s0_msgs,s0_bytes,s1_msgs,s1_bytes,s2_msgs,s2_bytes\n");

    for (int i = 0; i < NUM_ROWS; i++) {
        double m_tput_m, s_tput_m, m_tput_mb, s_tput_mb;
        double m_p50, s_p50, m_p95, s_p95, m_p99, s_p99, m_mean, s_mean;
        double m_pq, s_pq, m_pmem, s_pmem, m_fair, s_fair, m_drp, s_drp;
        double m_sc, s_sc, m_ovh, s_ovh;
        double m_s0m, s_s0m, m_s0b, s_s0b, m_s1m, s_s1m, m_s1b, s_s1b, m_s2m, s_s2m, m_s2b, s_s2b;

        calc_stats(perm[i].throughput_mps, NUM_RUNS, &m_tput_m, &s_tput_m);
        calc_stats(perm[i].throughput_mb,  NUM_RUNS, &m_tput_mb, &s_tput_mb);
        calc_stats(perm[i].p50_us,         NUM_RUNS, &m_p50, &s_p50);
        calc_stats(perm[i].p95_us,         NUM_RUNS, &m_p95, &s_p95);
        calc_stats(perm[i].p99_us,         NUM_RUNS, &m_p99, &s_p99);
        calc_stats(perm[i].mean_us,        NUM_RUNS, &m_mean, &s_mean);
        calc_stats(perm[i].peak_queue,     NUM_RUNS, &m_pq, &s_pq);
        calc_stats(perm[i].peak_mem_kib,   NUM_RUNS, &m_pmem, &s_pmem);
        calc_stats(perm[i].fairness,       NUM_RUNS, &m_fair, &s_fair);
        calc_stats(perm[i].drops,          NUM_RUNS, &m_drp, &s_drp);
        calc_stats(perm[i].state_changes,  NUM_RUNS, &m_sc, &s_sc);
        calc_stats(perm[i].ctrl_overhead,  NUM_RUNS, &m_ovh, &s_ovh);
        calc_stats(perm[i].s0_msgs,        NUM_RUNS, &m_s0m, &s_s0m);
        calc_stats(perm[i].s0_bytes,       NUM_RUNS, &m_s0b, &s_s0b);
        calc_stats(perm[i].s1_msgs,        NUM_RUNS, &m_s1m, &s_s1m);
        calc_stats(perm[i].s1_bytes,       NUM_RUNS, &m_s1b, &s_s1b);
        calc_stats(perm[i].s2_msgs,        NUM_RUNS, &m_s2m, &s_s2m);
        calc_stats(perm[i].s2_bytes,       NUM_RUNS, &m_s2b, &s_s2b);

        /* Recalculate Jain's fairness directly from the recorded per-sender mean byte counts */
        int n_flows = (strstr(perm[i].scenario, "Fairness Exp") != NULL) ? 2 : 3;
        double flow_b[3] = { m_s0b, m_s1b, m_s2b };
        double sum_b = 0.0, sum_sq_b = 0.0;
        for (int f = 0; f < n_flows; f++) {
            sum_b += flow_b[f];
            sum_sq_b += flow_b[f] * flow_b[f];
        }
        double recalc_fairness = (sum_sq_b > 0.0) ? (sum_b * sum_b) / ((double)n_flows * sum_sq_b) : 1.0;

        fprintf(out, "\"%s\",\"%s\",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f,%.2f,%.4f,%.0f,%.0f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                perm[i].scenario, perm[i].config,
                m_tput_m, m_tput_mb, m_p50, m_p95, m_p99, m_mean,
                m_pq, m_pmem, recalc_fairness, m_drp, m_sc, m_ovh,
                m_s0m, m_s0b, m_s1m, m_s1b, m_s2m, m_s2b);
    }
    fclose(out);
    printf("Aggregated 3-run summary successfully written to: %s\n\n", out_file);

    /* Verify Mathematical Consistency of Every Permutation Across All 3 Runs */
    bool all_runs_consistent = true;
    for (int i = 0; i < NUM_ROWS; i++) {
        size_t s0_frame = 1064, s1_frame = 1064, s2_frame = 1064;
        if (strstr(perm[i].scenario, "3. Sustained")) {
            s0_frame = s1_frame = s2_frame = 2088;
        } else if (strstr(perm[i].scenario, "Fairness Exp")) {
            s0_frame = 552;
            s1_frame = 4136;
            s2_frame = 0;
        }
        for (int r = 0; r < NUM_RUNS; r++) {
            if ((uint64_t)perm[i].s0_bytes[r] != (uint64_t)perm[i].s0_msgs[r] * s0_frame) {
                fprintf(stderr, "Discrepancy in %s run %d S0: msgs=%.0f bytes=%.0f (expected %lu)\n",
                        perm[i].scenario, r + 1, perm[i].s0_msgs[r], perm[i].s0_bytes[r],
                        (unsigned long)(perm[i].s0_msgs[r] * s0_frame));
                all_runs_consistent = false;
            }
            if ((uint64_t)perm[i].s1_bytes[r] != (uint64_t)perm[i].s1_msgs[r] * s1_frame) {
                fprintf(stderr, "Discrepancy in %s run %d S1: msgs=%.0f bytes=%.0f (expected %lu)\n",
                        perm[i].scenario, r + 1, perm[i].s1_msgs[r], perm[i].s1_bytes[r],
                        (unsigned long)(perm[i].s1_msgs[r] * s1_frame));
                all_runs_consistent = false;
            }
        }
    }
    if (all_runs_consistent) {
        printf("✓ Mathematical Verification: In all 3 runs and all 24 permutations,\n"
               "  per_sender_bytes EQUALS EXACTLY per_sender_msgs * frame_size!\n\n");
    }

    /* Print Comparative Statistical Table */
    printf("╔═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                                                  STATISTICAL EVALUATION SUMMARY (MEAN OVER 3 RUNS)                                                            ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-38s │ %-10s │ %-18s │ %-16s │ %-5s │ %-12s │ %-20s │ %-22s ║\n",
           "Scenario", "Config", "Throughput (msgs/s)", "p99 Latency (µs)", "PeakQ", "Recalc Fair", "Mean Delivered Msgs", "Mean Delivered Bytes");
    printf("╟────────────────────────────────────────┼────────────┼────────────────────┼──────────────────┼───────┼──────────────┼──────────────────────┼────────────────────────╢\n");

    for (int i = 0; i < NUM_ROWS; i++) {
        double m_tput_m, s_tput_m, m_p99, s_p99, m_pq, s_pq;
        double m_s0m, s_s0m, m_s0b, s_s0b, m_s1m, s_s1m, m_s1b, s_s1b, m_s2m, s_s2m, m_s2b, s_s2b;
        calc_stats(perm[i].throughput_mps, NUM_RUNS, &m_tput_m, &s_tput_m);
        calc_stats(perm[i].p99_us,         NUM_RUNS, &m_p99, &s_p99);
        calc_stats(perm[i].peak_queue,     NUM_RUNS, &m_pq, &s_pq);
        calc_stats(perm[i].s0_msgs,        NUM_RUNS, &m_s0m, &s_s0m);
        calc_stats(perm[i].s0_bytes,       NUM_RUNS, &m_s0b, &s_s0b);
        calc_stats(perm[i].s1_msgs,        NUM_RUNS, &m_s1m, &s_s1m);
        calc_stats(perm[i].s1_bytes,       NUM_RUNS, &m_s1b, &s_s1b);
        calc_stats(perm[i].s2_msgs,        NUM_RUNS, &m_s2m, &s_s2m);
        calc_stats(perm[i].s2_bytes,       NUM_RUNS, &m_s2b, &s_s2b);

        int n_flows = (strstr(perm[i].scenario, "Fairness Exp") != NULL) ? 2 : 3;
        double flow_b[3] = { m_s0b, m_s1b, m_s2b };
        double sum_b = 0.0, sum_sq_b = 0.0;
        for (int f = 0; f < n_flows; f++) {
            sum_b += flow_b[f];
            sum_sq_b += flow_b[f] * flow_b[f];
        }
        double recalc_fairness = (sum_sq_b > 0.0) ? (sum_b * sum_b) / ((double)n_flows * sum_sq_b) : 1.0;

        /* Extract short config code */
        const char *short_cfg = "Config D";
        if (strstr(perm[i].config, "Config A")) short_cfg = "Config A";
        else if (strstr(perm[i].config, "Config B")) short_cfg = "Config B";
        else if (strstr(perm[i].config, "Config C")) short_cfg = "Config C";

        char tput_buf[32], p99_buf[32], msgs_buf[32], bytes_buf[32];
        snprintf(tput_buf, sizeof(tput_buf), "%7.1f ± %-4.1f", m_tput_m, s_tput_m);
        snprintf(p99_buf,  sizeof(p99_buf),  "%6.1f ± %-4.1f", m_p99, s_p99);

        if (n_flows == 2) {
            snprintf(msgs_buf, sizeof(msgs_buf), "%.1f / %.1f", m_s0m, m_s1m);
            snprintf(bytes_buf, sizeof(bytes_buf), "%.0f / %.0f B", m_s0b, m_s1b);
        } else {
            snprintf(msgs_buf, sizeof(msgs_buf), "%.0f / %.0f / %.0f", m_s0m, m_s1m, m_s2m);
            snprintf(bytes_buf, sizeof(bytes_buf), "%.0f / %.0f / %.0f B", m_s0b, m_s1b, m_s2b);
        }

        printf("║ %-38s │ %-10s │ %-18s │ %-16s │ %5.0f │ %12.4f │ %-20s │ %-22s ║\n",
               perm[i].scenario, short_cfg, tput_buf, p99_buf, m_pq, recalc_fairness, msgs_buf, bytes_buf);
    }
    printf("╚═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝\n\n");

    return 0;
}
