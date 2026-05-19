CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -g -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS =
BINDIR  = bin
TARGET  = $(BINDIR)/process-manager
TESTDIR = test_programs

TEST_SRCS = $(wildcard $(TESTDIR)/*.c)
TEST_BINS = $(TEST_SRCS:$(TESTDIR)/%.c=$(TESTDIR)/%.out)

.PHONY: all clean test test-programs directories build

all: directories $(TARGET)

$(TARGET): process-manager.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test-programs: directories $(TEST_BINS)

$(TESTDIR)/%.out: $(TESTDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $<

# Also compiles test programs as part of all
all: test-programs

directories:
	mkdir -p $(BINDIR) logs

test: all
	@echo ""
	@echo "=== Running process-manager (press Ctrl+C to stop) ==="
	./$(TARGET)

test-verbose: all
	@echo ""
	@echo "=== Running process-manager with verbose logging ==="
	./$(TARGET) -v

build: all
	@echo ""
	@echo "=== Build complete ==="
	@echo "  Target: $(TARGET)"
	@echo "  Tests:  $(TEST_BINS)"

clean:
	rm -rf $(BINDIR) $(TESTDIR)/*.out logs/*.log process-manager.exe

# ------- Diagnostics -------

.PHONY: ps-check pstree-check pgrep-check log-check

ps-check:
	@echo "=== process-manager processes ==="
	ps aux | grep -E '[p]rocess-manager|[s]leeper|[c]rasher|[s]tatus_printer' || echo "(none)"

pstree-check:
	@echo "=== process-manager tree ==="
	pstree -p $(shell pgrep -x process-manager 2>/dev/null) 2>/dev/null || echo "(process-manager not running)"

pgrep-check:
	@echo "=== pgrep -a process-manager ==="
	pgrep -a process-manager 2>/dev/null || echo "(not found)"
	@echo "=== pgrep -a sleeper ==="
	pgrep -a sleeper 2>/dev/null || echo "(not found)"
	@echo "=== pgrep -a crasher ==="
	pgrep -a crasher 2>/dev/null || echo "(not found)"
	@echo "=== pgrep -a status_printer ==="
	pgrep -a status_printer 2>/dev/null || echo "(not found)"

log-check:
	@echo "=== Last 30 lines of manager.log ==="
	tail -30 logs/manager.log 2>/dev/null || echo "(log empty or not found)"
