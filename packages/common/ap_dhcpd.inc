/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * ap_dhcpd.inc - the DHCP server a static-AP wpa_supplicant needs.
 *
 * Behaviour-identical extraction of packages/wifi1b/src/wifi1b_backend.inc
 * (w2_dhcpd_run() and its lease/packet helpers, lines 566-1045 there): the same
 * leases, the same option set, the same 3600/1800/3150 second timers, the same
 * log lines.  The WiFi daemon needs this because ConnMan's tether - which used to
 * supply DHCP - is refused by the vendor on this unit (rc=0, rejected=1, measured
 * for hours on 2026-11-25), so an AP has to be a static wpa_supplicant AP with
 * its own server.  Copying rather than re-deriving keeps the two units of code
 * identical until they are merged into one include.
 *
 * The includer must define, before this file:
 *
 *   #define AP_DHCP_IP            "10.42.0.1"
 *   #define AP_DHCP_CIDR          "10.42.0.1/24"
 *   #define AP_DHCP_NETMASK       "255.255.255.0"
 *   #define AP_DHCP_BROADCAST     "10.42.0.255"
 *   #define AP_DHCP_POOL_FIRST    10
 *   #define AP_DHCP_POOL_LAST     200
 *   #define AP_DHCP_LEASE_FILE    APP_DIR "/dhcp.leases"
 *   #define AP_DHCP_PID_FILE      APP_DIR "/dhcpd.pid"
 *   #define AP_DHCP_LOG_FILE      APP_DIR "/dhcpd.log"
 *   #define AP_DHCP_PROC_NAME     "wifi_dhcpd"
 *
 * and fill an ap_dhcp_ops_t with its own log/run/stop primitives, so this file
 * depends on neither daemon's globals:
 *
 *   typedef struct {
 *       void (*log)(const char *line);   // one line, already formatted
 *       int  (*run)(const char *cmd);    // shell command, exit status
 *       bool (*stop_requested)(void);
 *   } ap_dhcp_ops_t;
 *
 * Server loop:  apd_dhcpd_run(&ops)     (used by the "--dhcpd" mode)
 * Lifecycle:    apd_dhcpd_start(&ops) / apd_dhcpd_stop() / apd_dhcpd_running()
 * Interface:    apd_iface_set_static(&ops) / apd_iface_flush(&ops)
 * Leases:       apd_leases_count()
 */
#ifndef AP_DHCPD_INC
#define AP_DHCPD_INC

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stddef.h>
#include <stdarg.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif
#ifndef AP_DHCPD_UNUSED
#define AP_DHCPD_UNUSED __attribute__((unused))
#endif
#ifndef AP_DHCPD_PRINTF
#define AP_DHCPD_PRINTF(fmt_index, first_arg) \
    __attribute__((format(printf, fmt_index, first_arg)))
#endif

typedef struct {
    void (*log)(const char *line);
    int  (*run)(const char *cmd);
    bool (*stop_requested)(void);
} ap_dhcp_ops_t;

/* ------------------------------------------------------------------ */
/* self-contained helpers (no daemon dependency)                       */
/* ------------------------------------------------------------------ */

static AP_DHCPD_UNUSED int64_t apd_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static AP_DHCPD_UNUSED int apd_read_file(const char *path, char *out, size_t n)
{
    if (!out || !n) return -1;
    out[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) { out[0] = 0; return -1; }
    out[r] = 0;
    return (int)r;
}

static AP_DHCPD_UNUSED bool apd_write_atomic(const char *path, const char *text, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    size_t len = strlen(text);
    bool ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    if (!ok) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* Keep the server log from growing without bound across boots. */
static AP_DHCPD_UNUSED void apd_cap_log(const char *path, off_t max)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= max) return;
    unlink(path);
}

static AP_DHCPD_UNUSED void apd_close_fds(int keep)
{
    for (int fd = 0; fd < 1024; fd++)
        if (fd != keep) close(fd);
}

static AP_DHCPD_UNUSED bool apd_proc_alive(pid_t pid)
{
    if (pid <= 1) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    char buf[512];
    if (apd_read_file(path, buf, sizeof(buf)) <= 0) return false;
    char *close_paren = strrchr(buf, ')');
    if (!close_paren || !close_paren[1]) return false;
    return close_paren[2] != 'Z' && close_paren[2] != 'X';
}

static AP_DHCPD_UNUSED int apd_pid_by_comm(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    int found = -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64], comm[128];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        if (apd_read_file(path, comm, sizeof(comm)) <= 0) continue;
        char *nl = strchr(comm, '\n');
        if (nl) *nl = 0;
        if (!strcmp(comm, name)) { found = atoi(de->d_name); break; }
    }
    closedir(d);
    return found;
}

static AP_DHCPD_UNUSED bool apd_parse_mac6(const char *s, unsigned char out[6])
{
    unsigned int b[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xff) return false;
        out[i] = (unsigned char)b[i];
    }
    return true;
}

static AP_DHCPD_UNUSED uint32_t apd_ip4(const char *s)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return 0;
    return ntohl(a.s_addr);
}

static AP_DHCPD_UNUSED void apd_ip4_str(uint32_t ip, char *out, size_t n)
{
    struct in_addr a;
    a.s_addr = htonl(ip);
    inet_ntop(AF_INET, &a, out, n);
}

static AP_DHCPD_UNUSED AP_DHCPD_PRINTF(2, 3)
void apd_log(const ap_dhcp_ops_t *ops, const char *fmt, ...)
{
    if (!ops || !ops->log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ops->log(line);
}

/* ------------------------------------------------------------------ */
/* leases                                                              */
/* ------------------------------------------------------------------ */

struct apd_dhcp_packet {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} __attribute__((packed));

typedef struct {
    bool used;
    uint8_t mac[6];
    uint32_t ip;
    int64_t expires_ms;
} apd_lease_t;

#define APD_LEASE_SLOTS 64
static apd_lease_t g_apd_leases[APD_LEASE_SLOTS];

static int apd_dhcp_port(void)
{
    const char *e = getenv("SCOURGE_DHCP_TEST_PORT");
    int p = e ? atoi(e) : 67;
    return p > 0 && p < 65536 ? p : 67;
}

static int apd_dhcp_client_port(void)
{
    const char *e = getenv("SCOURGE_DHCP_TEST_CLIENT_PORT");
    int p = e ? atoi(e) : 68;
    return p > 0 && p < 65536 ? p : 68;
}

static int apd_dhcpd_pid(void)
{
    char buf[64];
    if (apd_read_file(AP_DHCP_PID_FILE, buf, sizeof(buf)) > 0) {
        int p = atoi(buf);
        if (p > 1 && apd_proc_alive(p)) {
            char path[64], comm[128];
            snprintf(path, sizeof(path), "/proc/%d/comm", p);
            if (apd_read_file(path, comm, sizeof(comm)) > 0) {
                char *nl = strchr(comm, '\n');
                if (nl) *nl = 0;
                if (!strcmp(comm, AP_DHCP_PROC_NAME)) return p;
            }
        }
    }
    return apd_pid_by_comm(AP_DHCP_PROC_NAME);
}

static AP_DHCPD_UNUSED bool apd_dhcpd_running(void)
{
    return apd_dhcpd_pid() > 0;
}

static AP_DHCPD_UNUSED int apd_leases_count(void)
{
    FILE *f = fopen(AP_DHCP_LEASE_FILE, "r");
    if (!f) return 0;
    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f))
        if (line[0] && line[0] != '#') n++;
    fclose(f);
    return n;
}

static void apd_lease_write(void)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", AP_DHCP_LEASE_FILE, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    int64_t now = apd_now_ms();
    fprintf(f, "# mac ip expires_unix\n");
    for (int i = 0; i < APD_LEASE_SLOTS; i++) {
        if (!g_apd_leases[i].used || g_apd_leases[i].expires_ms <= now) continue;
        char ip[32];
        apd_ip4_str(g_apd_leases[i].ip, ip, sizeof(ip));
        fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x %s %lld\n",
                g_apd_leases[i].mac[0], g_apd_leases[i].mac[1],
                g_apd_leases[i].mac[2], g_apd_leases[i].mac[3],
                g_apd_leases[i].mac[4], g_apd_leases[i].mac[5],
                ip, (long long)(time(NULL) + (g_apd_leases[i].expires_ms - now) / 1000));
    }
    fclose(f);
    chmod(tmp, 0600);
    rename(tmp, AP_DHCP_LEASE_FILE);
}

/* Load unexpired leases after a restart: without this the server forgot every
 * address it had handed out and could give the same one to a second client.
 * Malformed, expired, out-of-pool and duplicate entries are ignored. */
static void apd_lease_load(const ap_dhcp_ops_t *ops)
{
    char buf[8192];
    if (apd_read_file(AP_DHCP_LEASE_FILE, buf, sizeof(buf)) <= 0) return;
    int64_t now = apd_now_ms();
    time_t wall = time(NULL);
    uint32_t base = apd_ip4(AP_DHCP_IP) & 0xffffff00U;
    uint32_t first = base + AP_DHCP_POOL_FIRST;
    uint32_t last = base + AP_DHCP_POOL_LAST;
    char *save = NULL;

    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char mac_text[32], ip_text[32];
        long long expiry = 0;
        unsigned char mac[6];
        struct in_addr address;
        uint32_t ip;

        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        if (sscanf(line, "%31s %31s %lld", mac_text, ip_text, &expiry) != 3) continue;
        if (expiry <= (long long)wall) continue;
        if (!apd_parse_mac6(mac_text, mac)) continue;
        if (inet_pton(AF_INET, ip_text, &address) != 1) continue;
        ip = ntohl(address.s_addr);
        if (ip < first || ip > last) continue;
        /* A corrupt far-future timestamp must not pin an address forever. */
        if (expiry > (long long)wall + 86400) expiry = (long long)wall + 86400;
        bool duplicate = false;
        int free_slot = APD_LEASE_SLOTS;
        for (int i = 0; i < APD_LEASE_SLOTS; i++) {
            if (!g_apd_leases[i].used) {
                if (free_slot == APD_LEASE_SLOTS) free_slot = i;
                continue;
            }
            if (!memcmp(g_apd_leases[i].mac, mac, 6) || g_apd_leases[i].ip == ip)
                duplicate = true;
        }
        if (duplicate || free_slot >= APD_LEASE_SLOTS) continue;
        g_apd_leases[free_slot].used = true;
        memcpy(g_apd_leases[free_slot].mac, mac, 6);
        g_apd_leases[free_slot].ip = ip;
        g_apd_leases[free_slot].expires_ms = now + (int64_t)(expiry - (long long)wall) * 1000;
    }
    apd_log(ops, "dhcpd: loaded persisted leases from %s", AP_DHCP_LEASE_FILE);
}

static uint32_t apd_lease_for_mac(const uint8_t mac[6], int64_t now)
{
    for (int i = 0; i < APD_LEASE_SLOTS; i++) {
        if (g_apd_leases[i].used && !memcmp(g_apd_leases[i].mac, mac, 6)) {
            /* A renewal refreshes the lifetime even before the old one expires. */
            g_apd_leases[i].expires_ms = now + 3600000;
            return g_apd_leases[i].ip;
        }
    }
    return 0;
}

static bool apd_ip_in_use(uint32_t ip, int64_t now)
{
    for (int i = 0; i < APD_LEASE_SLOTS; i++)
        if (g_apd_leases[i].used && g_apd_leases[i].ip == ip &&
            g_apd_leases[i].expires_ms > now) return true;
    return false;
}

static uint32_t apd_lease_alloc(const uint8_t mac[6], uint32_t requested, int64_t now)
{
    uint32_t base = apd_ip4(AP_DHCP_IP) & 0xffffff00U;
    uint32_t first = base + AP_DHCP_POOL_FIRST;
    uint32_t last = base + AP_DHCP_POOL_LAST;
    uint32_t existing = apd_lease_for_mac(mac, now);
    if (existing) { apd_lease_write(); return existing; }
    if (requested >= first && requested <= last && !apd_ip_in_use(requested, now))
        goto assign;
    for (uint32_t ip = first; ip <= last; ip++)
        if (!apd_ip_in_use(ip, now)) { requested = ip; goto assign; }
    return 0;
assign:
    for (int i = 0; i < APD_LEASE_SLOTS; i++) {
        if (!g_apd_leases[i].used || g_apd_leases[i].expires_ms <= now) {
            g_apd_leases[i].used = true;
            memcpy(g_apd_leases[i].mac, mac, 6);
            g_apd_leases[i].ip = requested;
            g_apd_leases[i].expires_ms = now + 3600000;
            apd_lease_write();
            return requested;
        }
    }
    return 0;
}

static void apd_lease_release(const uint8_t mac[6])
{
    for (int i = 0; i < APD_LEASE_SLOTS; i++) {
        if (g_apd_leases[i].used && !memcmp(g_apd_leases[i].mac, mac, 6)) {
            g_apd_leases[i].used = false;
            apd_lease_write();
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* packets                                                             */
/* ------------------------------------------------------------------ */

static uint8_t *apd_dhcp_opt_n(uint8_t *opts, size_t len, uint8_t code, uint8_t *out_len)
{
    size_t i = 0;
    if (out_len) *out_len = 0;
    while (i + 1 < len) {
        uint8_t c = opts[i++];
        if (c == 0) continue;
        if (c == 255) return NULL;
        uint8_t l = opts[i++];
        if (i + l > len) return NULL;
        if (c == code) {
            if (out_len) *out_len = l;
            return opts + i;
        }
        i += l;
    }
    return NULL;
}

static bool apd_dhcp_opt_u32(const struct apd_dhcp_packet *p, size_t n,
                             uint8_t code, uint32_t *out)
{
    size_t off = offsetof(struct apd_dhcp_packet, options) + 4;
    uint8_t len = 0;
    if (p == NULL || out == NULL || n <= off) return false;
    uint8_t *v = apd_dhcp_opt_n((uint8_t *)p->options + 4, n - off, code, &len);
    if (!v || len != 4) return false;
    uint32_t x;
    memcpy(&x, v, 4);
    *out = ntohl(x);
    return true;
}

static uint8_t apd_dhcp_msgtype(const struct apd_dhcp_packet *p, size_t n)
{
    size_t off = offsetof(struct apd_dhcp_packet, options) + 4;
    if (n <= off) return 0;
    uint8_t len = 0;
    uint8_t *v = apd_dhcp_opt_n((uint8_t *)p->options + 4, n - off, 53, &len);
    return v && len == 1 ? *v : 0;
}

static uint32_t apd_dhcp_requested(const struct apd_dhcp_packet *p, size_t n)
{
    uint32_t value = 0;
    if (apd_dhcp_opt_u32(p, n, 50, &value)) return value;
    return 0;
}

static bool apd_dhcp_server_id(const struct apd_dhcp_packet *p, size_t n, uint32_t *out)
{
    return apd_dhcp_opt_u32(p, n, 54, out);
}

static size_t apd_dhcp_put(uint8_t *dst, size_t cap, uint8_t code, uint8_t len, const void *data)
{
    if (cap < (size_t)len + 2) return 0;
    dst[0] = code;
    dst[1] = len;
    if (len) memcpy(dst + 2, data, len);
    return (size_t)len + 2;
}

static void apd_dhcp_send(int fd, const struct apd_dhcp_packet *req,
                          uint8_t type, uint32_t yiaddr, bool ack)
{
    struct apd_dhcp_packet rep;
    memset(&rep, 0, sizeof(rep));
    rep.op = 2;
    rep.htype = 1;
    rep.hlen = 6;
    rep.xid = req->xid;
    rep.flags = req->flags;
    rep.ciaddr = req->ciaddr;
    rep.yiaddr = htonl(yiaddr);
    rep.siaddr = htonl(apd_ip4(AP_DHCP_IP));
    memcpy(rep.chaddr, req->chaddr, 6);
    rep.options[0] = 99; rep.options[1] = 130; rep.options[2] = 83; rep.options[3] = 99;
    size_t off = 4;
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 53, 1, &type);
    uint32_t server = htonl(apd_ip4(AP_DHCP_IP));
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 54, 4, &server);
    uint32_t lease = htonl(3600);
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 51, 4, &lease);
    uint32_t renew = htonl(1800);
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 58, 4, &renew);
    uint32_t rebind = htonl(3150);
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 59, 4, &rebind);
    uint32_t mask = htonl(apd_ip4(AP_DHCP_NETMASK));
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 1, 4, &mask);
    uint32_t router = htonl(apd_ip4(AP_DHCP_IP));
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 3, 4, &router);
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 6, 4, &router);
    uint32_t bcast = htonl(apd_ip4(AP_DHCP_BROADCAST));
    off += apd_dhcp_put(rep.options + off, sizeof(rep.options) - off, 28, 4, &bcast);
    rep.options[off++] = 255;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(apd_dhcp_client_port());
    if (getenv("SCOURGE_DHCP_TEST_LOOPBACK")) to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (req->ciaddr) to.sin_addr.s_addr = req->ciaddr;
    else to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    (void)ack;
    sendto(fd, &rep, offsetof(struct apd_dhcp_packet, options) + off, 0,
           (struct sockaddr *)&to, sizeof(to));
}

/* ------------------------------------------------------------------ */
/* server                                                              */
/* ------------------------------------------------------------------ */

static AP_DHCPD_UNUSED bool apd_dhcpd_run(const ap_dhcp_ops_t *ops)
{
    prctl(PR_SET_NAME, AP_DHCP_PROC_NAME, 0, 0, 0);
    apd_lease_load(ops);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "wlan0", 6) != 0)
        apd_log(ops, "dhcpd: SO_BINDTODEVICE failed: %s", strerror(errno));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(apd_dhcp_port());
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        apd_log(ops, "dhcpd: bind udp/67 failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    apd_log(ops, "dhcpd: serving %s pool .%d-.%d on wlan0",
            AP_DHCP_IP, AP_DHCP_POOL_FIRST, AP_DHCP_POOL_LAST);
    int64_t last_clean = apd_now_ms();
    while (!(ops->stop_requested && ops->stop_requested())) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 1000);
        int64_t now = apd_now_ms();
        if (now - last_clean > 60000) {
            last_clean = now;
            apd_lease_write();
        }
        if (pr <= 0) continue;
        uint8_t buf[1500];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < (ssize_t)(offsetof(struct apd_dhcp_packet, options) + 4)) continue;
        struct apd_dhcp_packet *p = (struct apd_dhcp_packet *)buf;
        if (p->op != 1 || p->htype != 1 || p->hlen != 6) continue;
        uint8_t type = apd_dhcp_msgtype(p, (size_t)n);
        if (!type) continue;
        uint32_t requested = apd_dhcp_requested(p, (size_t)n);
        uint32_t sid = 0;
        bool has_sid = apd_dhcp_server_id(p, (size_t)n, &sid);
        char mac[32], ips[32];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 p->chaddr[0], p->chaddr[1], p->chaddr[2],
                 p->chaddr[3], p->chaddr[4], p->chaddr[5]);
        if (type == 1) { /* DISCOVER */
            uint32_t ip = apd_lease_alloc(p->chaddr, requested, now);
            if (!ip) { apd_log(ops, "dhcpd: pool exhausted for %s", mac); continue; }
            apd_ip4_str(ip, ips, sizeof(ips));
            apd_log(ops, "dhcpd: DISCOVER %s -> OFFER %s", mac, ips);
            apd_dhcp_send(fd, p, 2, ip, false);
        } else if (type == 3) { /* REQUEST */
            if (has_sid && sid != apd_ip4(AP_DHCP_IP)) continue;
            uint32_t ip = apd_lease_for_mac(p->chaddr, now);
            if (!ip && requested) ip = apd_lease_alloc(p->chaddr, requested, now);
            if (!ip) {
                apd_log(ops, "dhcpd: REQUEST %s but no lease; NAK", mac);
                apd_dhcp_send(fd, p, 6, 0, false);
                continue;
            }
            apd_ip4_str(ip, ips, sizeof(ips));
            apd_log(ops, "dhcpd: REQUEST %s -> ACK %s", mac, ips);
            apd_dhcp_send(fd, p, 5, ip, true);
            apd_lease_write();
        } else if (type == 7) { /* RELEASE */
            apd_log(ops, "dhcpd: RELEASE %s", mac);
            apd_lease_release(p->chaddr);
        } else if (type == 8) { /* INFORM */
            uint32_t ip = apd_ip4(AP_DHCP_IP);
            apd_log(ops, "dhcpd: INFORM %s -> ACK", mac);
            apd_dhcp_send(fd, p, 5, ip, true);
        }
    }
    close(fd);
    return true;
}

static AP_DHCPD_UNUSED bool apd_dhcpd_start(const ap_dhcp_ops_t *ops)
{
    if (apd_dhcpd_running()) return true;
    apd_cap_log(AP_DHCP_LOG_FILE, 1024 * 1024);
    pid_t pid = fork();
    if (pid == 0) {
        if (setsid() < 0) _exit(126);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_IGN);
        apd_close_fds(-1);
        int logfd = open(AP_DHCP_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (logfd >= 0) {
            fchmod(logfd, 0600);
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            close(logfd);
        }
        apd_dhcpd_run(ops);
        _exit(0);
    }
    if (pid <= 0) return false;
    char pidbuf[64];
    snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)pid);
    apd_write_atomic(AP_DHCP_PID_FILE, pidbuf, 0600);
    for (int i = 0; i < 20; i++) {
        if (apd_dhcpd_running()) return true;
        if (!apd_proc_alive(pid)) break;
        usleep(100000);
    }
    return false;
}

static AP_DHCPD_UNUSED void apd_dhcpd_stop(void)
{
    int pid = apd_dhcpd_pid();
    if (pid > 1) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 10 && apd_proc_alive(pid); i++) usleep(100000);
        if (apd_proc_alive(pid)) kill(pid, SIGKILL);
        waitpid(pid, NULL, WNOHANG);
    }
    unlink(AP_DHCP_PID_FILE);
}

/* ------------------------------------------------------------------ */
/* interface addressing                                                */
/* ------------------------------------------------------------------ */

/* Family-specific on purpose: a bare "ip addr flush" removes the IPv6
 * link-local address the AP needs to be reported ready. */
static AP_DHCPD_UNUSED void apd_iface_flush(const ap_dhcp_ops_t *ops)
{
    if (!ops || !ops->run) return;
    if (ops->run("ip -4 addr flush dev wlan0 2>/dev/null") != 0)
        ops->run("ifconfig wlan0 0.0.0.0 2>/dev/null");
}

static AP_DHCPD_UNUSED bool apd_iface_set_static(const ap_dhcp_ops_t *ops)
{
    if (!ops || !ops->run) return false;
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "ip addr add %s dev wlan0 2>/dev/null", AP_DHCP_CIDR);
    if (ops->run(cmd) == 0) {
        ops->run("ip link set wlan0 up 2>/dev/null");
        return true;
    }
    snprintf(cmd, sizeof(cmd), "ifconfig wlan0 %s netmask %s up 2>/dev/null",
             AP_DHCP_IP, AP_DHCP_NETMASK);
    return ops->run(cmd) == 0;
}

#endif /* AP_DHCPD_INC */
