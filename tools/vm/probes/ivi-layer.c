/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */

/* TARGET: Ioniq 5 Gen5W_L, 260128 (EU) */

/* Research-only ivi_controller client: put an existing ivi surface into an
 * existing vendor WM layer and commit the change. No OEM binary is modified. */
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
static void slp(long sec) { long ts[2] = { sec, 0 }; sc3(35, (long)ts, 0, 0); }
static int seq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

struct wl_display; struct wl_proxy; struct wl_interface;
struct wl_message { const char *name; const char *signature; const struct wl_interface **types; };
extern struct wl_display *wl_display_connect(const char *);
extern int wl_display_roundtrip(struct wl_display *);
extern int wl_display_dispatch(struct wl_display *);
extern int wl_display_dispatch_pending(struct wl_display *);
extern int wl_proxy_add_listener(void *, void (**)(void), void *);
extern void wl_proxy_marshal(struct wl_proxy *, u32, ...);
extern struct wl_proxy *wl_proxy_marshal_constructor(struct wl_proxy *, u32, const struct wl_interface *, ...);
extern int wl_display_flush(struct wl_display *);
extern const struct wl_interface wl_registry_interface;
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

static void *ctrl;
static u32 layers[64];
static int nlayers;
static const struct wl_interface *iface_surface;
static const struct wl_interface *iface_layer;
static const struct wl_interface *iface_controller;
static struct wl_display *g_disp;
static u32 g_layer = 107;
static struct wl_proxy *g_layers[8];
static int g_nlayers;

static void reg_global(void *d, void *reg, u32 name, const char *iface, u32 version) {
    (void)d; (void)version;
    if (iface && seq(iface, "ivi_controller") == 0 && !ctrl) {
        ctrl = wl_proxy_marshal_constructor((struct wl_proxy *)reg, 0,
            iface_controller, name, iface, 1, (void *)0);
    }
}
static void reg_remove(void *d, void *reg, u32 name) { (void)d; (void)reg; (void)name; }
static void ctl_screen(void *d, void *p, u32 id, void *np) { (void)d; (void)p; (void)np; wrnum("SCREEN ", id); }
static void ctl_layer(void *d, void *p, u32 id) {
    (void)d; (void)p;
    if (nlayers < 64) layers[nlayers++] = id;
    wrnum("LAYER ", id);
}
static void add_surface_to_layers(u32 sid) {
    struct wl_proxy *surf = wl_proxy_marshal_constructor((struct wl_proxy *)ctrl, 2,
        iface_surface, sid, (void *)0);
    if (!surf) return;
    wrnum("ADD_SURFACE ", sid);
    for (int i = 0; i < g_nlayers; i++) {
        wl_proxy_marshal(g_layers[i], 8, surf);
    }
    wl_proxy_marshal((struct wl_proxy *)ctrl, 0);
    wl_proxy_marshal(surf, 0, (u32)1);
    wl_proxy_marshal(surf, 3, (i32)0, (i32)0, (i32)1920, (i32)720);
    wl_proxy_marshal((struct wl_proxy *)ctrl, 0);
    wl_display_flush(g_disp);
    wr("ADDED_SURFACE\n", 14);
}
static void ctl_surface(void *d, void *p, u32 id) {
    (void)d; (void)p;
    wrnum("SURFACE ", id);
    if (id != 100 && id != 1) add_surface_to_layers(id);
}
static void ctl_error(void *d, void *p, i32 a, i32 b, const char *msg) {
    (void)d; (void)p; (void)b;
    wrnum("ERROR ", a);
    if (msg) { usize n = 0; while (msg[n]) n++; wr(msg, n); wr("\n", 1); }
}

__attribute__((used)) void start_c2(long argc, char **argv) {
    long want = 107;
    if (argc > 1 && argv && argv[1]) {
        long v = 0; char *p = argv[1];
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (v > 0) want = v;
    }
    g_layer = (u32)want;
    wr("WATCH_START\n", 12);
    wrnum("WANT_LAYER ", want);
    void *h = dlopen("/app/lib/libivi-platform-interface.so.1", 2);
    if (!h) h = dlopen("libivi-platform-interface.so.1", 2);
    if (!h) { wr("DLOPEN_FAIL\n", 12); return; }
    const struct wl_interface *ictl = (const struct wl_interface *)dlsym(h, "ivi_controller_interface");
    iface_surface = (const struct wl_interface *)dlsym(h, "ivi_controller_surface_interface");
    iface_layer = (const struct wl_interface *)dlsym(h, "ivi_controller_layer_interface");
    if (!ictl || !iface_surface || !iface_layer) { wr("DSYM_FAIL\n", 10); return; }
    iface_controller = ictl;
    struct wl_display *disp = wl_display_connect((void *)0);
    if (!disp) { wr("CONNECT_FAIL\n", 13); return; }
    g_disp = disp;
    wr("CONNECT_OK\n", 11);
    struct wl_proxy *reg = wl_proxy_marshal_constructor((struct wl_proxy *)disp, 1,
        &wl_registry_interface, (void *)0);
    void *rl[2] = { (void *)reg_global, (void *)reg_remove };
    wl_proxy_add_listener(reg, (void (**)(void))rl, (void *)0);
    wl_display_roundtrip(disp);
    if (!ctrl) { wr("NO_IVI_CONTROLLER\n", 18); return; }
    void *cl[4] = { (void *)ctl_screen, (void *)ctl_layer, (void *)ctl_surface, (void *)ctl_error };
    wl_proxy_add_listener((struct wl_proxy *)ctrl, (void (**)(void))cl, (void *)0);
    wl_display_roundtrip(disp);
    /* create proxies for the requested layer and the application-layer id */
    u32 lay[2] = { (u32)want, 4000 };
    for (int i = 0; i < 2; i++) {
        struct wl_proxy *lp = wl_proxy_marshal_constructor((struct wl_proxy *)ctrl, 1,
            iface_layer, lay[i], (i32)0, (i32)0, (void *)0);
        if (lp) {
            wl_proxy_marshal(lp, 0, (u32)1);
            wl_proxy_marshal(lp, 3, (i32)0, (i32)0, (i32)1920, (i32)720);
            g_layers[g_nlayers++] = lp;
            wrnum("LAYER_PROXY ", lay[i]);
        }
    }
    wl_display_flush(disp);
    /* watch for new surfaces for ~60s */
    for (int i = 0; i < 600; i++) {
        wl_display_flush(disp);
        if (wl_display_dispatch(disp) < 0) break;
    }
    wr("WATCH_END\n", 10);
}
__asm__(
".global _start\n"
"_start:\n"
"xor %rbp,%rbp\n"
"mov (%rsp),%rdi\n"
"lea 8(%rsp),%rsi\n"
"and $-16,%rsp\n"
"call start_c2\n"
"mov $60,%eax\n"
"xor %edi,%edi\n"
"syscall\n"
"hlt\n");
