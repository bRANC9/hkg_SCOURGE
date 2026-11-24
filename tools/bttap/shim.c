/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors */
/* SPDX-License-Identifier: AGPL-3.0-only */
/* LD_PRELOAD tap for the vendor Bluetooth daemon.
 *
 * The head unit has no kernel HCI device (no /sys/class/bluetooth): vbtd owns
 * /dev/ttyS0 and speaks HCI itself, so BlueZ cannot coexist and we have no
 * hci_mon to watch.  This shim records every read/write on that device so the
 * vendor stack can be decoded offline (tools/bttap/hci_decode.py) and, more
 * importantly, so we can see whether it carries an RFCOMM/SDP channel with the
 * Android Auto UUID.
 *
 * Build (host, static like everything we ship):
 *   musl-gcc -O2 -shared -fPIC -o libbttap.so shim.c -ldl
 * Run: BT_TAP_OUT=/rw_data/bt_tap.bin BT_TAP_DEV=/dev/ttyS0 \
 *      LD_PRELOAD=/rw_data/bt/libbttap.so <the vendor daemon's own command line>
 * Each record is a 1-byte direction ('<' read, '>' write), a 4-byte little
 * endian length, then the bytes - exactly the H4 stream the decoder expects.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int tap_fd = -1;
static int tap_out = -1;
static const char *tap_dev;

static void init_out(void)
{
    if (tap_out >= 0) return;
    const char *p = getenv("BT_TAP_OUT");
    tap_out = p ? open(p, O_WRONLY | O_CREAT | O_APPEND, 0600) : -2;
}

static void record(char dir, const void *buf, ssize_t n)
{
    if (n <= 0) return;
    init_out();
    if (tap_out < 0) return;
    unsigned char hdr[5];
    hdr[0] = (unsigned char)dir;
    hdr[1] = (unsigned char)(n & 0xff);
    hdr[2] = (unsigned char)((n >> 8) & 0xff);
    hdr[3] = (unsigned char)((n >> 16) & 0xff);
    hdr[4] = (unsigned char)((n >> 24) & 0xff);
    ssize_t w = 0;
    while (w < 5) {
        ssize_t r = write(tap_out, hdr + w, (size_t)(5 - w));
        if (r <= 0) return;
        w += r;
    }
    w = 0;
    const char *p = buf;
    while (w < n) {
        ssize_t r = write(tap_out, p + w, (size_t)(n - w));
        if (r <= 0) return;
        w += r;
    }
}

static int is_tap_path(const char *path)
{
    if (!path) return 0;
    tap_dev = getenv("BT_TAP_DEV");
    if (!tap_dev) tap_dev = "/dev/ttyS0";
    return strcmp(path, tap_dev) == 0;
}

int open(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    if (!real) real = dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = real(path, flags, mode);
    if (fd >= 0 && is_tap_path(path)) tap_fd = fd;
    return fd;
}

/* musl #defines open64 to open; glibc declares it as a symbol.  Guard on the
 * macro so the wrapper only exists where it is a real symbol. */
#ifndef open64
/* glibc has a separate open64 symbol; musl #defines it to open, so wrapping it
 * there would be a redefinition (and pointless: the alias is already tapped). */
int open64(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    if (!real) real = dlsym(RTLD_NEXT, "open64");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = real(path, flags, mode);
    if (fd >= 0 && is_tap_path(path)) tap_fd = fd;
    return fd;
}
#endif

ssize_t read(int fd, void *buf, size_t n)
{
    static ssize_t (*real)(int, void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "read");
    ssize_t r = real(fd, buf, n);
    if (fd == tap_fd) record('<', buf, r);
    return r;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    static ssize_t (*real)(int, const void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "write");
    ssize_t r = real(fd, buf, n);
    if (fd == tap_fd) record('>', buf, r);
    return r;
}
