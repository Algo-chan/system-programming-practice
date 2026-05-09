CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -g -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS =
INCDIR  = -Iinclude
SRCDIR  = src
OBJDIR  = obj
BINDIR  = bin
TARGET  = $(BINDIR)/process-manager

SRCS    = $(SRCDIR)/main.c          \
          $(SRCDIR)/process_manager.c \
          $(SRCDIR)/signal_handler.c   \
          $(SRCDIR)/logger.c           \
          $(SRCDIR)/daemon.c           \
          $(SRCDIR)/config.c

OBJS    = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

.PHONY: all clean directories

all: directories $(TARGET)

directories:
	@if not exist "$(OBJDIR)" mkdir "$(OBJDIR)"
	@if not exist "$(BINDIR)" mkdir "$(BINDIR)"
	@if not exist "logs" mkdir logs

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) $(INCDIR) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(INCDIR) -o $@ $^ $(LDFLAGS)

clean:
	@if exist "$(OBJDIR)" rmdir /s /q "$(OBJDIR)"
	@if exist "$(BINDIR)" rmdir /s /q "$(BINDIR)"
	@del /q logs\*.log 2>nul || ver>nul
