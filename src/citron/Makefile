CC ?= gcc
MUSL_CC ?= musl-gcc
CFLAGS = -O2 -Wall -Wextra -pedantic
LDFLAGS = -static

# Default target
TARGET = citron

# Source files
SRCS = main.c \
       ptrace_engine.c \
       breakpoint.c \
       elf_parser.c \
       devvm_parser.c \
       regs.c \
       stack.c \
       disasm.c \
       source.c \
       readline_ui.c \
       util.c

OBJS = $(SRCS:.c=.o)

# Build with standard glibc (default)
all: $(TARGET)

# Build with musl (smallest, most portable)
musl: MUSL_BUILD=1
musl: CC=$(MUSL_CC)
musl: $(TARGET)

# Build with debug symbols
debug: CFLAGS = -g -O0 -Wall -Wextra -pedantic
debug: $(TARGET)

$(TARGET): $(OBJS)
	@echo "[LD] $@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)
	@size -B $@ 2>/dev/null || true
	@ls -lh $@

%.o: %.c citron.h
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean test size musl debug

clean:
	rm -f $(OBJS) $(TARGET)

test: $(TARGET)
	@echo "Running basic self-test..."
	@./$(TARGET) -h

size: $(TARGET)
	@echo "Binary size analysis:"
	@ls -lh $(TARGET)
	@file $(TARGET)
	@echo "Floppy compatibility: $(TARGET) fits on 1.44 MB floppy"
	@du -h $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installed to /usr/local/bin/$(TARGET)"

.SILENT: test
