# Makefile for Adaptive IPC Backpressure and Fairness Control in Linux
# Target Platform: Linux x86-64
# Language: Pure C/C++ (GCC / G++)

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -g -I./include
LDFLAGS := -lm

BIN_DIR := bin
SRC_DIR := src
TEST_DIR := tests
RES_DIR := results

# Core Object Files
CORE_OBJS := $(SRC_DIR)/common.o \
             $(SRC_DIR)/queue.o \
             $(SRC_DIR)/controller.o \
             $(SRC_DIR)/scheduler.o

# Main Binaries
BROKER_BIN   := $(BIN_DIR)/broker
SENDER_BIN   := $(BIN_DIR)/sender
RECEIVER_BIN := $(BIN_DIR)/receiver

# Test & Benchmark Binaries
TEST_PROTO_BIN := $(TEST_DIR)/test_protocol_unit
TEST_QUEUE_BIN := $(TEST_DIR)/test_queue_unit
TEST_CTRL_BIN  := $(TEST_DIR)/test_controller_unit
TEST_SCHED_BIN := $(TEST_DIR)/test_scheduler_unit
TEST_ROBUST_BIN:= $(TEST_DIR)/test_robustness
BENCH_SUITE_BIN:= $(TEST_DIR)/benchmark_suite
TEST_COMPUTE_BIN:= $(TEST_DIR)/compute_summary

TEST_BINS := $(TEST_PROTO_BIN) $(TEST_QUEUE_BIN) $(TEST_CTRL_BIN) $(TEST_SCHED_BIN) $(TEST_ROBUST_BIN) $(TEST_COMPUTE_BIN)

.PHONY: all binaries tests test benchmark run clean help sanitizer plots validate

all: binaries $(TEST_BINS) $(BENCH_SUITE_BIN)
	@echo ""
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║  BUILD COMPLETE: Adaptive IPC Backpressure & Fairness System     ║"
	@echo "║    Broker Daemon   : $(BROKER_BIN)                                  ║"
	@echo "║    Sender Client   : $(SENDER_BIN)                                  ║"
	@echo "║    Receiver Process: $(RECEIVER_BIN)                                ║"
	@echo "║    Benchmark Suite : $(BENCH_SUITE_BIN)                         ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""

binaries: $(BIN_DIR) $(BROKER_BIN) $(SENDER_BIN) $(RECEIVER_BIN)

$(BIN_DIR):
	@mkdir -p $(BIN_DIR) $(RES_DIR)

# ── Main Executables ──────────────────────────────────────────────

$(BROKER_BIN): $(SRC_DIR)/broker.c $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SENDER_BIN): $(SRC_DIR)/sender.c $(SRC_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(RECEIVER_BIN): $(SRC_DIR)/receiver.c $(SRC_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Object Files ──────────────────────────────────────────────────

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Unit Tests & Benchmarks ───────────────────────────────────────

$(TEST_PROTO_BIN): $(TEST_DIR)/test_protocol_unit.c $(SRC_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_QUEUE_BIN): $(TEST_DIR)/test_queue_unit.c $(SRC_DIR)/queue.o $(SRC_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_CTRL_BIN): $(TEST_DIR)/test_controller_unit.c $(SRC_DIR)/controller.o $(SRC_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_SCHED_BIN): $(TEST_DIR)/test_scheduler_unit.c $(SRC_DIR)/scheduler.o $(SRC_DIR)/queue.o $(SRC_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_ROBUST_BIN): $(TEST_DIR)/test_robustness.c $(SRC_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BENCH_SUITE_BIN): $(TEST_DIR)/benchmark_suite.c $(SRC_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_COMPUTE_BIN): $(TEST_DIR)/compute_summary.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Execution Workflows ───────────────────────────────────────────

tests: binaries $(TEST_BINS)
	@echo "Running Protocol Unit Tests..."
	./$(TEST_PROTO_BIN)
	@echo "Running Queue Unit Tests..."
	./$(TEST_QUEUE_BIN)
	@echo "Running Controller Unit Tests..."
	./$(TEST_CTRL_BIN)
	@echo "Running Scheduler Unit Tests..."
	./$(TEST_SCHED_BIN)
	@echo "Running Robustness & Fault-Tolerance Tests..."
	./$(TEST_ROBUST_BIN)

test: tests

benchmark: binaries $(BENCH_SUITE_BIN)
	@mkdir -p $(RES_DIR)
	./$(BENCH_SUITE_BIN)

run: all tests benchmark

clean:
	rm -rf $(BIN_DIR) $(SRC_DIR)/*.o
	rm -f $(TEST_BINS) $(BENCH_SUITE_BIN)
	rm -f /tmp/adaptive_ipc_*.sock /tmp/ipc_bench_*.sock /tmp/test_adaptive_ipc_*.sock
	rm -f /tmp/bench_broker_*.csv /tmp/bench_recv_*.csv

sanitizer: clean
	@echo "Building and running all tests under AddressSanitizer & UndefinedBehaviorSanitizer..."
	@mkdir -p $(BIN_DIR) $(RES_DIR)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -O1 -o $(TEST_PROTO_BIN) $(TEST_DIR)/test_protocol_unit.c $(SRC_DIR)/common.c $(LDFLAGS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -O1 -o $(TEST_QUEUE_BIN) $(TEST_DIR)/test_queue_unit.c $(SRC_DIR)/queue.c $(SRC_DIR)/common.c $(LDFLAGS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -O1 -o $(TEST_CTRL_BIN) $(TEST_DIR)/test_controller_unit.c $(SRC_DIR)/controller.c $(SRC_DIR)/common.c $(LDFLAGS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -O1 -o $(TEST_SCHED_BIN) $(TEST_DIR)/test_scheduler_unit.c $(SRC_DIR)/scheduler.c $(SRC_DIR)/queue.c $(SRC_DIR)/common.c $(LDFLAGS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -O1 -o $(BROKER_BIN) $(SRC_DIR)/broker.c $(SRC_DIR)/common.c $(SRC_DIR)/queue.c $(SRC_DIR)/controller.c $(SRC_DIR)/scheduler.c $(LDFLAGS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -O1 -o $(SENDER_BIN) $(SRC_DIR)/sender.c $(SRC_DIR)/common.c $(LDFLAGS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -O1 -o $(RECEIVER_BIN) $(SRC_DIR)/receiver.c $(SRC_DIR)/common.c $(LDFLAGS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -O1 -o $(TEST_ROBUST_BIN) $(TEST_DIR)/test_robustness.c $(SRC_DIR)/common.c $(LDFLAGS)
	./$(TEST_PROTO_BIN)
	./$(TEST_QUEUE_BIN)
	./$(TEST_CTRL_BIN)
	./$(TEST_SCHED_BIN)
	./$(TEST_ROBUST_BIN)
	@echo "All unit & robustness tests passed cleanly under AddressSanitizer and UndefinedBehaviorSanitizer!"

plots:
	python3 scripts/generate_plots.py

validate:
	./scripts/validate_project.sh

help:
	@echo "Targets:"
	@echo "  all       : Build broker, sender, receiver, unit tests, and benchmark suite"
	@echo "  tests     : Compile and run all unit and robustness test suites"
	@echo "  benchmark : Run full 4-configuration 6-scenario evaluation harness"
	@echo "  sanitizer : Build and execute unit tests under AddressSanitizer & UndefinedBehaviorSanitizer"
	@echo "  plots     : Generate publication-quality figures in docs/figures/"
	@echo "  validate  : Run end-to-end project validation script"
	@echo "  run       : Build everything, run all tests, and execute full benchmark"
	@echo "  clean     : Clean binaries, object files, temporary sockets, and scratch CSVs"
