# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# GNU make predefines CC=cc, so "?=" never fired and every daemon was linked
# against the host glibc - which on a rolling distro is x86-64-v3 and SIGILLs on
# the head unit's Atom-class CPU. Override only make's built-in default.
# Hard-pin the compiler for the recipe: make's built-in CC and any exported CC
# both produced a glibc -static binary, which faults on the head unit's CPU.
MUSL_CC := $(shell command -v musl-gcc >/dev/null 2>&1 && echo musl-gcc || echo cc)
# Baseline ISA on purpose: the head unit is an Atom-class x86-64 with SSE4.2 but
# no AVX/BMI2, and a build whose code assumes more dies with SIGILL (exit 132) at
# startup - which is exactly what every daemon did on 2026-11-22. Keep -march at
# the x86-64 baseline and tune generic; put host-only tuning in EXTRA_CFLAGS.
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -march=x86-64 -mtune=generic $(EXTRA_CFLAGS)
LDFLAGS ?= -static
BUILD_ID ?= $(shell sha256sum tweaks_daemon.c | cut -c1-12)
CPPFLAGS += -DTWEAKS_BUILD_ID='"$(BUILD_ID)"'

all: tweaks_daemon wrandr_wrapper

tweaks_daemon: tweaks_daemon.c
	$(MUSL_CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

wrandr_wrapper: wrandr_wrapper.c
	$(MUSL_CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

persist:
	sh ../scripts/build_target_helper.sh

clean:
	rm -f tweaks_daemon wrandr_wrapper tweaks_persist

.PHONY: all persist clean
