CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -g -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS =
BINDIR  = bin
SRC     = process-manager.c
TESTDIR = test_programs

TARGET_SUPERVISOR = $(BINDIR)/process-manager
TARGET_CLEAN      = $(BINDIR)/process-manager-clean

TEST_SRCS = $(wildcard $(TESTDIR)/*.c)
TEST_BINS = $(TEST_SRCS:$(TESTDIR)/%.c=$(TESTDIR)/%.out)

.PHONY: all clean test test-verbose clean-mode test-clean test-clean-verbose \
        directories build supervisor-mode

# Default: build BOTH modes
all: directories $(TARGET_SUPERVISOR) $(TARGET_CLEAN) test-programs

# Supervisor mode (default, no extra flag)
supervisor-mode: directories $(TARGET_SUPERVISOR) test-programs

$(TARGET_SUPERVISOR): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Clean mode (compile with -DCLEAN_MODE)
clean-mode: directories $(TARGET_CLEAN) test-programs

$(TARGET_CLEAN): $(SRC)
	$(CC) $(CFLAGS) -DCLEAN_MODE -o $@ $< $(LDFLAGS)

test-programs: $(TEST_BINS)

$(TESTDIR)/%.out: $(TESTDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $<

directories:
	mkdir -p $(BINDIR) logs

# ---- Test targets ----

test: supervisor-mode
	@echo ""
	@echo "=== Supervisor Mode (default) ==="
	@echo "  Press Ctrl+C to stop"
	@echo "  Try: kill -HUP \$$\$$ in another terminal"
	@echo ""
	./$(TARGET_SUPERVISOR)

test-verbose: supervisor-mode
	@echo ""
	@echo "=== Supervisor Mode (verbose) ==="
	./$(TARGET_SUPERVISOR) -v

test-clean: clean-mode
	@echo ""
	@echo "=== Clean Mode (PR_SET_PDEATHSIG) ==="
	@echo "  Press Ctrl+C to stop"
	@echo "  Try: kill -9 \$$\$$  (children die automatically)"
	@echo ""
	./$(TARGET_CLEAN)

test-clean-verbose: clean-mode
	@echo ""
	@echo "=== Clean Mode (verbose) ==="
	./$(TARGET_CLEAN) -v

build: all
	@echo ""
	@echo "=== Build complete ==="
	@echo "  supervisor: $(TARGET_SUPERVISOR)"
	@echo "  clean:      $(TARGET_CLEAN)"
	@echo "  tests:      $(TEST_BINS)"

clean:
	rm -rf $(BINDIR) $(TESTDIR)/*.out logs/*.log process-manager.exe

# ======== Diagnostics ========

.PHONY: ps-check pstree-check pgrep-check log-check log-tail kill-all

ps-check:
	@echo "=== process-manager processes ==="
	ps aux | grep -E '[p]rocess-manager|[s]leeper|[c]rasher|[s]tatus_printer' || echo "(none)"

pstree-check:
	@echo "=== process tree ==="
	PID=$$(pgrep -x process-manager 2>/dev/null); \
	if [ -n "$$PID" ]; then pstree -p $$PID; \
	else echo "(not running)"; fi

pgrep-check:
	@for p in process-manager sleeper crasher status_printer; do \
		echo "  $$p: $$(pgrep -a $$p 2>/dev/null || echo '(not found)')"; \
	done

log-check:
	@echo "=== Last 40 lines of logs/manager.log ==="
	tail -40 logs/manager.log 2>/dev/null || echo "(empty)"

log-tail:
	tail -f logs/manager.log 2>/dev/null || echo "(not found)"

kill-all:
	-pkill -x process-manager process-manager-clean 2>/dev/null; \
	pkill -x sleeper crasher status_printer 2>/dev/null; \
	echo "Killed all"; sleep 1; make ps-check

# ======== PDEATHSIG demo ========

.PHONY: demo-pdeathsig

demo-pdeathsig: test-clean-verbose
	@echo ""
	@echo "In another terminal, run:  kill -9 \$$\$$(pgrep -x process-manager-clean)"
	@echo "Then:  pgrep -a sleeper   (should show nothing)"
