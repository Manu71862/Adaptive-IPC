#!/usr/bin/env python3
"""
generate_plots.py — Publication-Quality Performance Visualization

Adaptive IPC Backpressure and Fairness Control in Linux

Generates high-resolution figures for technical documentation and reports
from results/benchmark_summary.csv.
"""

import os
import sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

OUTPUT_DIR = "docs/figures"
SUMMARY_CSV = "results/benchmark_summary.csv"

# Configuration Colors (Colorblind-friendly palette)
CONFIG_COLORS = {
    "Config A (Fixed Window, FIFO)": "#4C72B0",     # Muted Blue
    "Config B (Fixed Threshold, FIFO)": "#DD8452",  # Muted Orange
    "Config C (Adaptive EWMA, FIFO)": "#55A868",    # Muted Green
    "Config D (Adaptive EWMA, DRR)": "#C44E52",     # Muted Red
}

CONFIG_LABELS = {
    "Config A (Fixed Window, FIFO)": "Config A (Fixed Window, FIFO)",
    "Config B (Fixed Threshold, FIFO)": "Config B (Fixed Threshold, FIFO)",
    "Config C (Adaptive EWMA, FIFO)": "Config C (Adaptive EWMA, FIFO)",
    "Config D (Adaptive EWMA, DRR)": "Config D (Adaptive EWMA, DRR)",
}

SHORT_SCENARIOS = [
    "1. Normal",
    "2. Bursts",
    "3. Overload",
    "4. Fair A (Vol)",
    "5. Fair B (Backlog)",
    "6. Stall"
]

def load_data():
    if not os.path.exists(SUMMARY_CSV):
        print(f"Error: {SUMMARY_CSV} not found", file=sys.stderr)
        sys.exit(1)
    df = pd.read_csv(SUMMARY_CSV)
    return df

def setup_plot_style():
    plt.rcParams.update({
        'font.size': 11,
        'font.family': 'sans-serif',
        'axes.labelsize': 12,
        'axes.titlesize': 13,
        'xtick.labelsize': 10,
        'ytick.labelsize': 10,
        'legend.fontsize': 10,
        'figure.titlesize': 14,
        'figure.dpi': 300,
        'savefig.dpi': 300,
        'axes.grid': True,
        'grid.alpha': 0.35,
        'grid.linestyle': '--'
    })

def plot_throughput(df):
    setup_plot_style()
    fig, ax = plt.subplots(figsize=(10, 5.5))

    scenarios = df['scenario'].unique()
    configs = list(CONFIG_COLORS.keys())
    x = np.arange(len(scenarios))
    width = 0.20

    for i, cfg in enumerate(configs):
        subset = df[df['config'] == cfg]
        tput = [subset[subset['scenario'] == s]['throughput_msgs_per_s'].values[0] for s in scenarios]
        rects = ax.bar(x + (i - 1.5) * width, tput, width, label=CONFIG_LABELS[cfg],
                       color=CONFIG_COLORS[cfg], edgecolor='black', linewidth=0.6)

    ax.set_ylabel("Throughput (messages / second)")
    ax.set_title("Message Throughput Across 6 Benchmark Scenarios & 4 Configurations")
    ax.set_xticks(x)
    ax.set_xticklabels(SHORT_SCENARIOS)
    ax.legend(loc='upper right', framealpha=0.95)
    ax.set_ylim(0, 4200)

    fig.tight_layout()
    path = os.path.join(OUTPUT_DIR, "throughput_comparison.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"Generated: {path}")

def plot_latency_percentiles(df):
    setup_plot_style()
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5.5))

    # Subplot 1: Median p50 Latency (Scenarios 1-5)
    scenarios_normal = df['scenario'].unique()[:5]
    configs = list(CONFIG_COLORS.keys())
    x = np.arange(len(scenarios_normal))
    width = 0.20

    for i, cfg in enumerate(configs):
        subset = df[df['config'] == cfg]
        p50 = [subset[subset['scenario'] == s]['p50_us'].values[0] for s in scenarios_normal]
        ax1.bar(x + (i - 1.5) * width, p50, width, label=CONFIG_LABELS[cfg],
                color=CONFIG_COLORS[cfg], edgecolor='black', linewidth=0.6)

    ax1.set_ylabel("Median Latency p50 (µs)")
    ax1.set_title("Median Transit Latency (Scenarios 1–5)")
    ax1.set_xticks(x)
    ax1.set_xticklabels(SHORT_SCENARIOS[:5])
    ax1.legend(loc='upper left', framealpha=0.95)

    # Subplot 2: Tail p99 Latency (Log Scale to include Scenario 6 stall)
    scenarios_all = df['scenario'].unique()
    x_all = np.arange(len(scenarios_all))
    for i, cfg in enumerate(configs):
        subset = df[df['config'] == cfg]
        p99 = [subset[subset['scenario'] == s]['p99_us'].values[0] for s in scenarios_all]
        ax2.bar(x_all + (i - 1.5) * width, p99, width, label=CONFIG_LABELS[cfg],
                color=CONFIG_COLORS[cfg], edgecolor='black', linewidth=0.6)

    ax2.set_ylabel("Tail Latency p99 (µs) [Log Scale]")
    ax2.set_title("Tail Latency p99 with Receiver Stall (Scenario 6)")
    ax2.set_yscale('log')
    ax2.set_xticks(x_all)
    ax2.set_xticklabels(SHORT_SCENARIOS)
    ax2.axhline(250000, color='darkred', linestyle=':', linewidth=1.5, label='250 ms Stall Threshold')
    ax2.legend(loc='upper left', framealpha=0.95)

    fig.tight_layout()
    path = os.path.join(OUTPUT_DIR, "latency_percentiles.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"Generated: {path}")

def plot_queue_depth(df):
    setup_plot_style()
    fig, ax = plt.subplots(figsize=(10, 5.5))

    scenarios = df['scenario'].unique()
    configs = list(CONFIG_COLORS.keys())
    x = np.arange(len(scenarios))
    width = 0.20

    for i, cfg in enumerate(configs):
        subset = df[df['config'] == cfg]
        qdepth = [subset[subset['scenario'] == s]['peak_queue_msgs'].values[0] for s in scenarios]
        ax.bar(x + (i - 1.5) * width, qdepth, width, label=CONFIG_LABELS[cfg],
               color=CONFIG_COLORS[cfg], edgecolor='black', linewidth=0.6)

    ax.axhline(256, color='red', linestyle='--', linewidth=1.8, label='Global Capacity Bound (256 msgs)')
    ax.axhline(64, color='orange', linestyle=':', linewidth=1.4, label='Per-Sender Capacity Bound (64 msgs)')

    ax.set_ylabel("Peak Queue Depth (messages)")
    ax.set_title("Peak Queue Depth Across Benchmark Scenarios vs Architectural Bounds")
    ax.set_xticks(x)
    ax.set_xticklabels(SHORT_SCENARIOS)
    ax.legend(loc='upper right', framealpha=0.95)
    ax.set_ylim(0, 300)

    fig.tight_layout()
    path = os.path.join(OUTPUT_DIR, "queue_depth_comparison.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"Generated: {path}")

def plot_jains_fairness(df):
    setup_plot_style()
    fig, ax = plt.subplots(figsize=(10, 5.5))

    scenarios = df['scenario'].unique()
    configs = list(CONFIG_COLORS.keys())
    x = np.arange(len(scenarios))
    width = 0.20

    for i, cfg in enumerate(configs):
        subset = df[df['config'] == cfg]
        fair = [subset[subset['scenario'] == s]['jain_fairness'].values[0] for s in scenarios]
        rects = ax.bar(x + (i - 1.5) * width, fair, width, label=CONFIG_LABELS[cfg],
                       color=CONFIG_COLORS[cfg], edgecolor='black', linewidth=0.6)
        # Label values on Scenario 5
        val_s5 = fair[4]
        ax.text(x[4] + (i - 1.5) * width, val_s5 + 0.02, f"{val_s5:.4f}",
                ha='center', va='bottom', fontsize=8, rotation=90)

    ax.set_ylabel("Jain's Byte Fairness Index")
    ax.set_title("Bandwidth Fairness: Strict FIFO Inequity vs DRR Byte Parity")
    ax.set_xticks(x)
    ax.set_xticklabels(SHORT_SCENARIOS)
    ax.axhline(1.0, color='darkgreen', linestyle='--', alpha=0.7, label='Ideal Fairness (J = 1.0000)')
    ax.legend(loc='lower left', framealpha=0.95)
    ax.set_ylim(0.50, 1.15)

    fig.tight_layout()
    path = os.path.join(OUTPUT_DIR, "jains_fairness_comparison.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"Generated: {path}")

def plot_scenario5_byte_distribution(df):
    setup_plot_style()
    fig, ax = plt.subplots(figsize=(9, 5.5))

    s5 = df[df['scenario'].str.contains("5. Fairness Exp B")]
    configs = list(CONFIG_COLORS.keys())
    short_cfg_labels = ["Config A\n(Fixed Win, FIFO)", "Config B\n(Fixed Thr, FIFO)",
                        "Config C\n(Adapt EWMA, FIFO)", "Config D\n(Adapt EWMA, DRR)"]

    s0_bytes = [s5[s5['config'] == c]['s0_bytes'].values[0] / 1024.0 for c in configs]
    s1_bytes = [s5[s5['config'] == c]['s1_bytes'].values[0] / 1024.0 for c in configs]

    x = np.arange(len(configs))
    width = 0.35

    rects1 = ax.bar(x - width/2, s0_bytes, width, label='Sender 0 (512B Frame, Control)',
                    color='#3274A1', edgecolor='black', linewidth=0.8)
    rects2 = ax.bar(x + width/2, s1_bytes, width, label='Sender 1 (4096B Frame, Bulk Data)',
                    color='#E1812C', edgecolor='black', linewidth=0.8)

    for r in rects1:
        h = r.get_height()
        ax.annotate(f"{h:.1f} KiB", xy=(r.get_x() + r.get_width()/2, h),
                    xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=9)
    for r in rects2:
        h = r.get_height()
        ax.annotate(f"{h:.1f} KiB", xy=(r.get_x() + r.get_width()/2, h),
                    xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=9)

    ax.set_ylabel("Total Delivered Volume (KiB)")
    ax.set_title("Scenario 5 (Continuous Backlog): Delivered Bytes per Sender Flow")
    ax.set_xticks(x)
    ax.set_xticklabels(short_cfg_labels)
    ax.legend(loc='upper right', framealpha=0.95)
    ax.set_ylim(0, 480)

    fig.tight_layout()
    path = os.path.join(OUTPUT_DIR, "scenario5_byte_distribution.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"Generated: {path}")

def plot_scenario5_packet_ratio(df):
    setup_plot_style()
    fig, ax = plt.subplots(figsize=(9, 5.5))

    s5 = df[df['scenario'].str.contains("5. Fairness Exp B")]
    configs = list(CONFIG_COLORS.keys())
    short_cfg_labels = ["Config A\n(Fixed Win, FIFO)", "Config B\n(Fixed Thr, FIFO)",
                        "Config C\n(Adapt EWMA, FIFO)", "Config D\n(Adapt EWMA, DRR)"]

    s0_msgs = [s5[s5['config'] == c]['s0_msgs'].values[0] for c in configs]
    s1_msgs = [s5[s5['config'] == c]['s1_msgs'].values[0] for c in configs]

    x = np.arange(len(configs))
    width = 0.35

    rects1 = ax.bar(x - width/2, s0_msgs, width, label='Sender 0 (Small 552B Messages)',
                    color='#4C72B0', edgecolor='black', linewidth=0.8)
    rects2 = ax.bar(x + width/2, s1_msgs, width, label='Sender 1 (Large 4136B Messages)',
                    color='#C44E52', edgecolor='black', linewidth=0.8)

    for r in rects1:
        h = r.get_height()
        ax.annotate(f"{int(round(h))}", xy=(r.get_x() + r.get_width()/2, h),
                    xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=9)
    for r in rects2:
        h = r.get_height()
        ax.annotate(f"{int(round(h))}", xy=(r.get_x() + r.get_width()/2, h),
                    xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=9)

    # Annotate ratio for Config A and Config D
    ax.annotate("Packet Ratio ≈ 1:1\n(Severe Byte Inequity)",
                xy=(0, 105), xytext=(0, 140),
                ha='center', fontsize=9,
                bbox=dict(boxstyle="round,pad=0.3", fc="#ffcccc", ec="red", lw=1))
    ax.annotate("Packet Ratio ≈ 7.57:1\n(Perfect Byte Parity)",
                xy=(3, 178), xytext=(3, 195),
                ha='center', fontsize=9,
                bbox=dict(boxstyle="round,pad=0.3", fc="#ccffcc", ec="green", lw=1))

    ax.set_ylabel("Delivered Message Count (out of 200 total)")
    ax.set_title("Scenario 5 (Continuous Backlog): Message Dispatch Ratio (1:1 FIFO vs 7.5:1 DRR)")
    ax.set_xticks(x)
    ax.set_xticklabels(short_cfg_labels)
    ax.legend(loc='upper center', framealpha=0.95)
    ax.set_ylim(0, 230)

    fig.tight_layout()
    path = os.path.join(OUTPUT_DIR, "scenario5_packet_ratio.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"Generated: {path}")

def plot_stall_recovery(df):
    setup_plot_style()
    fig, ax = plt.subplots(figsize=(9, 5.5))

    s6 = df[df['scenario'].str.contains("6. Receiver Slowdown")]
    configs = list(CONFIG_COLORS.keys())
    short_cfg_labels = ["Config A\n(Fixed Window)", "Config B\n(Fixed Threshold)",
                        "Config C\n(Adaptive EWMA)", "Config D\n(Adaptive DRR)"]

    p50 = [s6[s6['config'] == c]['p50_us'].values[0] / 1000.0 for c in configs]
    p99 = [s6[s6['config'] == c]['p99_us'].values[0] / 1000.0 for c in configs]

    x = np.arange(len(configs))
    width = 0.35

    rects1 = ax.bar(x - width/2, p50, width, label='Median Latency p50 (ms)',
                    color='#55A868', edgecolor='black', linewidth=0.8)
    rects2 = ax.bar(x + width/2, p99, width, label='Tail Latency p99 (ms) [Outage]',
                    color='#C44E52', edgecolor='black', linewidth=0.8)

    ax.axhline(250, color='darkred', linestyle='--', linewidth=1.5, label='Injected 250 ms Receiver Stall')

    for r in rects1:
        h = r.get_height()
        ax.annotate(f"{h:.1f} ms", xy=(r.get_x() + r.get_width()/2, h),
                    xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=9)
    for r in rects2:
        h = r.get_height()
        ax.annotate(f"{h:.1f} ms", xy=(r.get_x() + r.get_width()/2, h),
                    xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=9)

    ax.set_ylabel("Latency (milliseconds)")
    ax.set_title("Scenario 6: Backpressure Response & Recovery Under 250 ms Receiver Outage")
    ax.set_xticks(x)
    ax.set_xticklabels(short_cfg_labels)
    ax.legend(loc='upper left', framealpha=0.95)
    ax.set_ylim(0, 310)

    fig.tight_layout()
    path = os.path.join(OUTPUT_DIR, "receiver_stall_latency.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"Generated: {path}")

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    df = load_data()
    print("Generating publication-quality figures...")
    plot_throughput(df)
    plot_latency_percentiles(df)
    plot_queue_depth(df)
    plot_jains_fairness(df)
    plot_scenario5_byte_distribution(df)
    plot_scenario5_packet_ratio(df)
    plot_stall_recovery(df)
    print("All figures successfully created in docs/figures/!")

if __name__ == "__main__":
    main()
