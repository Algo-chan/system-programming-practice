CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -g -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS =
INCDIR  = -Iinclude
SRCDIR  = src
OBJDIR  = obj
BINDIR  = bin
TARGET  = $(BINDIR)/process-manager
TESTDIR = test_programs

SRCS    = $(SRCDIR)/main.c          \
          $(SRCDIR)/process_manager.c \
          $(SRCDIR)/signal_handler.c   \
          $(SRCDIR)/logger.c           \
          $(SRCDIR)/daemon.c           \
          $(SRCDIR)/config.c

OBJS    = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

TEST_SRCS = $(wildcard $(TESTDIR)/*.c)
TEST_BINS = $(TEST_SRCS:$(TESTDIR)/%.c=$(TESTDIR)/%.out)

.PHONY: all clean test directories

all: directories $(TARGET) $(TEST_BINS)

directories:
	mkdir -p $(OBJDIR) $(BINDIR) logs

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) $(INCDIR) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(INCDIR) -o $@ $^ $(LDFLAGS)

$(TESTDIR)/%.out: $(TESTDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	@echo ""
	@echo "=== Running process-manager (press Ctrl+C to stop) ==="
	./$(TARGET)

clean:
	rm -rf $(OBJDIR) $(BINDIR) $(TESTDIR)/*.out logs/*.log
