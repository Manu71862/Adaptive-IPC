/**
 * common.c — Implementation of Core Utilities, Socket I/O, and Statistics
 *
 * Adaptive IPC Backpressure and Fairness Control in Linux
 */
#include "common.h"
#include <stdarg.h>

static LogLevel g_log_level = LOG_LEVEL_INFO;

void log_set_level(LogLevel level) {
    g_log_level = level;
}

void log_msg(LogLevel level, const char *fmt, ...) {
    if (level < g_log_level) return;

    char time_str[32];
    double now_sec = get_time_sec();
    snprintf(time_str, sizeof(time_str), "[%10.4f]", now_sec);

    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "%s ", time_str);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

/* ── Socket Helpers (AF_UNIX, SOCK_SEQPACKET) ────────────────────── */

int ipc_create_listener_socket(const char *socket_path) {
    if (!socket_path) return -EINVAL;

    unlink(socket_path);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        LOG_E("Failed to create UNIX domain socket: %s", strerror(errno));
        return -errno;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E("Failed to bind socket to '%s': %s", socket_path, strerror(errno));
        close(fd);
        return -errno;
    }

    if (listen(fd, 32) < 0) {
        LOG_E("Failed to listen on socket '%s': %s", socket_path, strerror(errno));
        close(fd);
        unlink(socket_path);
        return -errno;
    }

    return fd;
}

int ipc_connect_socket(const char *socket_path) {
    if (!socket_path) return -EINVAL;

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        LOG_E("Failed to create client socket: %s", strerror(errno));
        return -errno;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -errno;
    }

    return fd;
}

int ipc_set_nonblocking(int fd, bool nonblock) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -errno;

    if (nonblock) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    if (fcntl(fd, F_SETFL, flags) < 0) return -errno;
    return 0;
}

int ipc_send_msg(int fd, const IpcMessage *msg) {
    if (fd < 0 || !msg) return -EINVAL;

    size_t total_len = sizeof(IpcHeader) + msg->header.payload_len;
    if (total_len > sizeof(IpcMessage)) return -EMSGSIZE;

    ssize_t n = send(fd, msg, total_len, MSG_NOSIGNAL);
    if (n < 0) {
        return -errno;
    }
    return (int)n;
}

int ipc_recv_msg(int fd, IpcMessage *out_msg) {
    if (fd < 0 || !out_msg) return -EINVAL;

    ssize_t n = recv(fd, out_msg, sizeof(IpcMessage), 0);
    if (n == 0) {
        return 0; /* Orderly connection teardown (EOF) */
    }
    if (n < 0) {
        return -errno;
    }

    if ((size_t)n < sizeof(IpcHeader)) {
        LOG_W("Received undersized IPC frame (%zd bytes < header %zu bytes)", n, sizeof(IpcHeader));
        return -EBADMSG;
    }

    if (!ipc_msg_validate(out_msg)) {
        LOG_W("Invalid IPC message frame received (magic=0x%08x, type=%u, len=%u)",
              out_msg->header.magic, out_msg->header.msg_type, out_msg->header.payload_len);
        return -EBADMSG;
    }

    return (int)n;
}

void ipc_unlink_socket(const char *socket_path) {
    if (socket_path) {
        unlink(socket_path);
    }
}

/* ── Latency Sample Buffer & Percentiles ──────────────────────────── */

LatencySampleBuffer *sample_buffer_create(size_t initial_cap) {
    if (initial_cap == 0) initial_cap = 256;
    LatencySampleBuffer *sb = (LatencySampleBuffer *)malloc(sizeof(LatencySampleBuffer));
    if (!sb) return NULL;

    sb->values = (double *)malloc(initial_cap * sizeof(double));
    if (!sb->values) {
        free(sb);
        return NULL;
    }
    sb->count = 0;
    sb->capacity = initial_cap;
    return sb;
}

void sample_buffer_free(LatencySampleBuffer *sb) {
    if (sb) {
        if (sb->values) free(sb->values);
        free(sb);
    }
}

void sample_buffer_add(LatencySampleBuffer *sb, double val_us) {
    if (!sb) return;
    if (sb->count >= sb->capacity) {
        size_t new_cap = sb->capacity * 2;
        double *new_vals = (double *)realloc(sb->values, new_cap * sizeof(double));
        if (!new_vals) return;
        sb->values = new_vals;
        sb->capacity = new_cap;
    }
    sb->values[sb->count++] = val_us;
}

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

void sample_buffer_sort(LatencySampleBuffer *sb) {
    if (sb && sb->count > 1) {
        qsort(sb->values, sb->count, sizeof(double), compare_doubles);
    }
}

double sample_buffer_percentile(const LatencySampleBuffer *sb, double p) {
    if (!sb || sb->count == 0) return 0.0;
    if (p <= 0.0) return sb->values[0];
    if (p >= 100.0) return sb->values[sb->count - 1];

    double rank = (p / 100.0) * (double)(sb->count - 1);
    size_t low = (size_t)rank;
    size_t high = low + 1;
    if (high >= sb->count) high = sb->count - 1;

    double frac = rank - (double)low;
    return sb->values[low] + frac * (sb->values[high] - sb->values[low]);
}

double sample_buffer_mean(const LatencySampleBuffer *sb) {
    if (!sb || sb->count == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < sb->count; i++) sum += sb->values[i];
    return sum / (double)sb->count;
}

/*
 * calculate_jains_fairness — Jain's Fairness Index over n flows.
 *
 * All n flows are counted in the denominator regardless of whether they
 * received any bytes, exactly matching the standard formula:
 *
 *   J(x1,...,xn) = (sum xi)^2 / (n * sum xi^2)
 *
 * A flow with xi = 0 contributes 0 to the numerator sums but n in the
 * denominator, so a 2-flow system where one flow gets nothing returns
 * J = 0.5000, not 1.0000.
 *
 * Special case: if all flows delivered 0 bytes, there is no work to
 * measure fairness over; return 1.0 with that understanding documented.
 */
double calculate_jains_fairness(const double *values, size_t n) {
    if (!values || n == 0) return 1.0;

    double sum = 0.0;
    double sum_sq = 0.0;

    for (size_t i = 0; i < n; i++) {
        /* Include ALL n entries — zeros remain zeros */
        if (values[i] > 0.0) {
            sum += values[i];
            sum_sq += values[i] * values[i];
        }
        /* values[i] == 0 contributes 0 to both sums but is counted by n */
    }

    /* All flows delivered 0 bytes: no data transferred, fairness is 1.0 by
     * convention (vacuously fair — no one got anything). */
    if (sum_sq == 0.0) return 1.0;

    return (sum * sum) / ((double)n * sum_sq);
}
