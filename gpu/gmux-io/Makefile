# Makefile — build gmux-io (a C helper for apple-gmux registers via /dev/port)
#
#   make            builds gmux-io
#   make clean      removes the binary
#
# Requirements: gcc, /dev/port (root at runtime)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

TARGET   = gmux-io

all: $(TARGET)

$(TARGET): gmux-io.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean