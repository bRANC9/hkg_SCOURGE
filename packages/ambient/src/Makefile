# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror
LDFLAGS ?= -static
LDLIBS ?= -lm

all: ambient_daemon

ambient_daemon: ambient_daemon.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f ambient_daemon

.PHONY: all clean
