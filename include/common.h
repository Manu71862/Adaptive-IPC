/**
 * common.h — Core Utilities, Time Measurement, Socket I/O, and Statistical Tools
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 */
#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#include "ipc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Logging ─────────────────────────────────────────────────────── */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_NONE  = 4
} LogLevel;

void log_set_level(LogLevel level);
void log_msg(LogLevel level, const char *fmt, ...);

#define LOG_D(fmt, ...) log_msg(LOG_LEVEL_DEBUG, "[DEBUG] " fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) log_msg(LOG_LEVEL_INFO,  "[INFO]  " fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) log_msg(LOG_LEVEL_WARN,  "[WARN]  " fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) log_msg(LOG_LEVEL_ERROR, "[ERROR] " fmt, ##__VA_ARGS__)

/* ── Monotonic Time Utilities ────────────────────────────────────── */

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static inline double get_time_ms(void) {
    return (double)get_time_ns() / 1e6;
}

static inline double get_time_sec(void) {
    return (double)get_time_ns() / 1e9;
}

static inline void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ── Socket Helpers (UNIX Domain SOCK_SEQPACKET) ─────────────────── */

int ipc_create_listener_socket(const char *socket_path);
int ipc_connect_socket(const char *socket_path);
int ipc_set_nonblocking(int fd, bool nonblock);
int ipc_send_msg(int fd, const IpcMessage *msg);
int ipc_recv_msg(int fd, IpcMessage *out_msg);
void ipc_unlink_socket(const char *socket_path);

/* ── Latency Percentiles & Statistics ────────────────────────────── */

typedef struct {
    double *values;
    size_t count;
    size_t capacity;
} LatencySampleBuffer;

LatencySampleBuffer *sample_buffer_create(size_t initial_cap);
void sample_buffer_free(LatencySampleBuffer *sb);
void sample_buffer_add(LatencySampleBuffer *sb, double val_us);
void sample_buffer_sort(LatencySampleBuffer *sb);
double sample_buffer_percentile(const LatencySampleBuffer *sb, double p); /* e.g. 50, 95, 99 */
double sample_buffer_mean(const LatencySampleBuffer *sb);

/** Calculates Jain's Fairness Index over n positive allocation/throughput values */
double calculate_jains_fairness(const double *values, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_H */
