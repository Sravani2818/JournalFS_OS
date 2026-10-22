# JournalFS build (C11 Strict)
#
#   make            build build/journalfs and all test executables
#   make test       functional, persistence, error handling, corruption, and bcache tests
#   make test-crash crash injection + recovery tests
#   make test-all   every test binary including stress test
#   make benchmark  storage comparison benchmarks + recovery benchmarks
#   make sanitize   rebuild with AddressSanitizer + UndefinedBehaviorSanitizer
#   make test-sanitize run all tests under ASan + UBSan
#   make clean

CC       ?= gcc
STD      ?= -std=c11
WARN     ?= -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wwrite-strings \
            -Wpointer-arith -Wcast-align -Wformat=2
OPT      ?= -O2 -g
CFLAGS   ?= $(STD) $(WARN) $(OPT)
CPPFLAGS += -Iinclude -Itests
LDFLAGS  ?=
LDLIBS   ?= -lreadline

BUILD    := build
BIN      := $(BUILD)/journalfs
TESTBIN  := $(BUILD)/fsstress

# All compiled test binaries (strict C11)
TEST_FUNCTIONAL := $(BUILD)/test_functional
TEST_ERRORS     := $(BUILD)/test_errors
TEST_PERSIST    := $(BUILD)/test_persistence
TEST_CORRUPT    := $(BUILD)/test_corruption
TEST_CRASH      := $(BUILD)/test_crash_recovery
TEST_BCACHE     := $(BUILD)/test_bcache
TEST_STRESS     := $(BUILD)/test_stress
TEST_COMPARE    := $(BUILD)/test_comparison
BENCH_BIN       := $(BUILD)/benchmark

TEST_BINS := \
	$(TEST_FUNCTIONAL) \
	$(TEST_ERRORS) \
	$(TEST_PERSIST) \
	$(TEST_CORRUPT) \
	$(TEST_CRASH) \
	$(TEST_BCACHE) \
	$(TEST_STRESS) \
	$(TEST_COMPARE) \
	$(BENCH_BIN)

SRCS := \
	src/util.c \
	src/checksum.c \
	src/crash.c \
	src/inode.c \
	src/permissions.c \
	src/directory.c \
	src/disk_image.c \
	src/checkpoint.c \
	src/journal.c \
	src/transaction.c \
	src/bcache.c \
	src/storage.c \
	src/filesystem.c \
	src/recovery.c \
	src/cli.c \
	src/main.c

CORE_OBJS := $(filter-out $(BUILD)/main.o,$(SRCS:src/%.c=$(BUILD)/%.o))
OBJS      := $(SRCS:src/%.c=$(BUILD)/%.o)
DEPS      := $(OBJS:.o=.d)

.PHONY: all clean test test-crash test-all smoke benchmark sanitize test-sanitize help

all: $(BIN) $(TESTBIN) $(TEST_BINS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	@cp -f $@ journalfs

# Standalone stress driver
$(TESTBIN): tests/fsstress.c $(CORE_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

# C11 test suite binaries
$(TEST_FUNCTIONAL): tests/test_functional.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_ERRORS): tests/test_errors.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_PERSIST): tests/test_persistence.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_CORRUPT): tests/test_corruption.c $(BUILD)/checksum.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_CRASH): tests/test_crash_recovery.c $(BUILD)/checksum.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BCACHE): tests/test_bcache.c $(BUILD)/bcache.o $(BUILD)/util.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_STRESS): tests/test_stress.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_COMPARE): tests/test_comparison.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(BENCH_BIN): benchmarks/benchmark.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

test: all
	@$(TEST_FUNCTIONAL)
	@$(TEST_ERRORS)
	@$(TEST_PERSIST)
	@$(TEST_CORRUPT)
	@$(TEST_BCACHE)

test-crash: all
	@$(TEST_CRASH)

test-compare: all
	@$(TEST_COMPARE)

test-all: test test-crash test-compare
	@$(TEST_STRESS)

smoke: all
	@$(TEST_FUNCTIONAL)

benchmark: all
	@$(BENCH_BIN)

sanitize: clean
	$(MAKE) OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" all
	@echo "sanitizer build complete: $(BIN); run 'make test-sanitize' to use it"

test-sanitize: sanitize
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
	    $(TEST_FUNCTIONAL)
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
	    $(TEST_ERRORS)
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
	    $(TEST_PERSIST)
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
	    $(TEST_CORRUPT)
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
	    $(TEST_BCACHE)
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
	    $(TEST_CRASH)
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
	    $(TEST_COMPARE)
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
	    $(TEST_STRESS)
	@echo "all suites passed under AddressSanitizer + UndefinedBehaviorSanitizer"
	@echo "run 'make' to rebuild the optimized binary"

clean:
	rm -rf $(BUILD) journalfs

help:
	@echo "targets: all test test-crash test-all benchmark sanitize test-sanitize clean"

-include $(DEPS)
