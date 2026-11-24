# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# The AA core is a library, not a daemon yet: only the transport-agnostic
# frame layer exists, so `make` builds objects and `make check` runs the
# standalone tests (normal build plus ASan/UBSan).
CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic -Wshadow

# Building for the head unit is not the host build.
#
# Measured on the unit (2026-11-22): a glibc-static binary died with SIGILL
# (code=dumped, signal=ILL) on the Atom-class CPU, because the static glibc
# start-up used ISA extensions the unit does not have.  Battery hit it first and
# moved to musl-gcc with -march=x86-64 -mtune=generic; anything we ship there
# follows the same rule.  musl is fetched by scripts/00_fetch_third_party.sh, so
# on a plain dev host this target simply says it is unavailable.
TARGET_CC := $(shell command -v musl-gcc 2>/dev/null || true)
TARGET_FLAGS ?= -O2 -std=c11 -march=x86-64 -mtune=generic -static-pie -Wall -Wextra
OBJS = aa_wire.o aa_channels.o aa_proto.o aa_control.o aa_tls.o aa_sensor.o aa_session.o \
	aa_aoap.o aa_btboot.o aa_mirror.o aa_ap.o aa_wireless.o

BUILD_ID = $(shell python3 ../wifi1b/scripts/build_id.py 2>/dev/null || echo dev)

TRANSPORT_OBJS = aa_tcp.o

all: $(OBJS) $(TRANSPORT_OBJS)

aa_wire.o: src/aa_wire.c src/aa_wire.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_wire.c

aa_channels.o: src/aa_channels.c src/aa_channels.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_channels.c

aa_proto.o: src/aa_proto.c src/aa_proto.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_proto.c

aa_control.o: src/aa_control.c src/aa_control.h src/aa_channels.h src/aa_proto.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_control.c

aa_tls.o: src/aa_tls.c src/aa_tls.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_tls.c

aa_sensor.o: src/aa_sensor.c src/aa_sensor.h src/aa_channels.h src/aa_proto.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_sensor.c

aa_session.o: src/aa_session.c src/aa_session.h src/aa_control.h src/aa_sensor.h src/aa_tls.h \
	src/aa_wire.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_session.c

aa_aoap.o: src/aa_aoap.c src/aa_aoap.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_aoap.c

aa_btboot.o: src/aa_btboot.c src/aa_btboot.h src/aa_proto.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_btboot.c

aa_mirror.o: src/aa_mirror.c src/aa_mirror.h src/aa_proto.h src/aa_channels.h src/aa_sensor.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_mirror.c

aa_ap.o: src/aa_ap.c src/aa_ap.h src/aa_btboot.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_ap.c

aa_wireless.o: src/aa_wireless.c src/aa_wireless.h src/aa_ap.h src/aa_btboot.h src/aa_session.h
	$(CC) $(CFLAGS) -c -o $@ src/aa_wireless.c

# The debug daemon is the only target that owns a socket; it is not installed
# by anything yet.
aa_daemon: daemon/aa_daemon.c $(OBJS)
	$(CC) $(CFLAGS) -DAA_DAEMON_BUILD_ID='"$(BUILD_ID)"' -o $@ daemon/aa_daemon.c \
		aa_ap.o aa_btboot.o aa_proto.o

aa_tcp.o: transport/aa_tcp.c transport/aa_tcp.h src/aa_session.h
	$(CC) $(CFLAGS) -c -o $@ transport/aa_tcp.c

# The deployable binary for the unit: musl, baseline ISA, static-pie.
target-aa_daemon:
	@if [ -z "$(TARGET_CC)" ]; then \
		echo "musl-gcc not found: run scripts/00_fetch_third_party.sh first"; exit 1; \
	fi
	$(TARGET_CC) $(TARGET_FLAGS) -DAA_DAEMON_BUILD_ID='"$(BUILD_ID)"' \
		-o aa_daemon.target daemon/aa_daemon.c src/aa_ap.c src/aa_btboot.c src/aa_proto.c

# The one measurement the on-unit route depends on: whether our process can own
# the SDP (L2CAP PSM 1) and AA RFCOMM (channel 8) sockets, or the vendor daemon
# does.  Everything else on that route is software we already have.
bt-owner: probes/bt_owner.c
	$(CC) $(CFLAGS) -o $@ probes/bt_owner.c

target-bt-owner:
	@if [ -z "$(TARGET_CC)" ]; then \
		echo "musl-gcc not found: run scripts/00_fetch_third_party.sh first"; exit 1; \
	fi
	$(TARGET_CC) $(TARGET_FLAGS) -o bt_owner.target probes/bt_owner.c

# Which binary exports the AA-critical symbols, without nm on the unit: the
# vendor daemons are ELF files, so their .dynsym is readable directly.
vbtd-api: probes/vbtd_api.c
	$(CC) $(CFLAGS) -o $@ probes/vbtd_api.c

target-vbtd-api:
	@if [ -z "$(TARGET_CC)" ]; then \
		echo "musl-gcc not found: run scripts/00_fetch_third_party.sh first"; exit 1; \
	fi
	$(TARGET_CC) $(TARGET_FLAGS) -o vbtd_api.target probes/vbtd_api.c

bt-owner-selftest:
	$(CC) $(CFLAGS) -DAA_BT_OWNER_SELFTEST -o bt_owner.selftest probes/bt_owner.c
	./bt_owner.selftest

check: bt-owner-selftest
	python3 tests/test_aa.py

clean:
	rm -f $(OBJS) $(TRANSPORT_OBJS) aa_daemon aa_daemon.target \
		bt-owner bt_owner.target bt_owner.selftest \
		vbtd-api vbtd_api.target

.PHONY: all check clean target-aa_daemon bt-owner target-bt-owner bt-owner-selftest \
	target-vbtd-api vbtd-api
