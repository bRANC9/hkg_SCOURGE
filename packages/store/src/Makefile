# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
LDFLAGS ?= -static
STORE_VERSION ?= 0.1.0
BUILD_ID ?= $(shell sha256sum store_daemon.c | cut -c1-12)
CPPFLAGS += -DSTORE_BUILD_ID='"$(BUILD_ID)"' -DSTORE_VERSION='"$(STORE_VERSION)"'

all: store_daemon

store_daemon: store_daemon.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f store_daemon

.PHONY: all clean
