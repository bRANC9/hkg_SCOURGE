/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */

/* TARGET: Ioniq 5 Gen5W_L, 260128 (EU) */

/* Research-only input-focus helper: bind the vendor ivi_input global and set
 * focus/acceptance for the app surface. No OEM binary is modified. The helper
 * is lab-only evidence; QMP clicks still do not reach the app in the current
 * probe, so this is not a stock input-focus fix.
 *
 * Usage: ivi-focus [surface-id]   (default 11000, the launcher surface id) */
typedef unsigned long usize;
typedef unsigned int u32;
typedef int i32;
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static void wr(const char *s, usize n) { sc3(1, 2, (long)s, (long)n); }
static void wrnum(const char *tag, long v) {
    char b[64]; int n = 0;
    while (*tag) b[n++] = *tag++;
    char t[16]; int m = 0;
    if (v < 0) { b[n++] = '-'; v = -v; }
    if (!v) t[m++] = '0';
    while (v) { t[m++] = '0' + v % 10; v /= 10; }
    while (m) b[n++] = t[--m];
    b[n++] = '\n';
    sc3(1, 2, (long)b, n);
}
static int seq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

struct wl_display; struct wl_proxy;
struct wl_message { const char *name; const char *signature; const struct wl_interface **types; };
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};
extern struct wl_display *wl_display_connect(const char *);
extern int wl_display_roundtrip(struct wl_display *);
extern int wl_display_flush(struct wl_display *);
extern int wl_proxy_add_listener(void *, void (**)(void), void *);
extern void wl_proxy_marshal(struct wl_proxy *, u32, ...);
extern struct wl_proxy *wl_proxy_marshal_constructor(struct wl_proxy *, u32,
    const struct wl_interface *, ...);
extern const struct wl_interface wl_registry_interface;

static struct wl_display *g_disp;
static struct wl_proxy *g_input;

/* The vendor input controller's interface data has:
 *   set_input_focus:      uui  (surface id, seat id, focus)
 *   set_input_acceptance: usi  (surface id, seat name, accepted)
 */
static const struct wl_interface *no_types[3] = { 0, 0, 0 };
static struct wl_message reqs[2] = {
    { "set_input_focus", "uui", no_types },
    { "set_input_acceptance", "usi", no_types },
};
static struct wl_interface iface_input = {
    "ivi_input", 1, 2, reqs, 0, 0,
};

static void reg_global(void *d, void *reg, u32 name, const char *iface, u32 version) {
    (void)d; (void)version;
    if (iface && seq(iface, "ivi_input") == 0 && !g_input) {
        g_input = wl_proxy_marshal_constructor((struct wl_proxy *)reg, 0,
            &iface_input, name, iface, 1, (void *)0);
    }
}
static void reg_remove(void *d, void *reg, u32 name) { (void)d; (void)reg; (void)name; }

__attribute__((used)) void start_focus(long argc, char **argv) {
    /* Surface id is the launcher's QT_WAYLAND_SET_SURFACE_ID (11000); allow an
     * explicit override so the same binary can target any app surface. */
    u32 surface = 11000;
    if (argc > 1 && argv && argv[1]) {
        long v = 0;
        char *p = argv[1];
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (v > 0) surface = (u32)v;
    }
    wr("FOCUS_START\n", 12);
    struct wl_display *disp = wl_display_connect((void *)0);
    if (!disp) { wr("CONNECT_FAIL\n", 13); return; }
    g_disp = disp;
    wr("CONNECT_OK\n", 11);
    struct wl_proxy *reg = wl_proxy_marshal_constructor((struct wl_proxy *)disp, 1,
        &wl_registry_interface, (void *)0);
    void *rl[2] = { (void *)reg_global, (void *)reg_remove };
    wl_proxy_add_listener(reg, (void (**)(void))rl, (void *)0);
    wl_display_roundtrip(disp);
    if (!g_input) { wr("NO_IVI_INPUT\n", 13); return; }
    wrnum("INPUT ", surface);
    /* target surface; QEMU seat name default, seat id 3 */
    wl_proxy_marshal(g_input, 1, surface, "default", (i32)1);
    wl_proxy_marshal(g_input, 0, surface, (u32)3, (i32)1);
    wl_display_flush(disp);
    wr("FOCUS_SET\n", 10);
    wr("FOCUS_END\n", 10);
}
__asm__(
".global _start\n"
"_start:\n"
"xor %rbp,%rbp\n"
"mov (%rsp),%rdi\n"
"lea 8(%rsp),%rsi\n"
"and $-16,%rsp\n"
"call start_focus\n"
"mov $60,%eax\n"
"xor %edi,%edi\n"
"syscall\n"
"hlt\n");
