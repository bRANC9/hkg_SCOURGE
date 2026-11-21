/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * store_daemon - the head-unit side of the SCOURGE Hub marketplace.
 *
 * It is the whole "logic" behind the future Marketplace tab: the QML only has
 * to call these endpoints and render the answers.
 *
 *   GET  /status                   state, firmware, pairing, job, installed[]
 *   POST /config?hub=URL[&allow_install=0|1]
 *   POST /pair?code=NNNNNN | ?token=...
 *   POST /refresh                  fetch + cache the signed index for our key
 *   GET  /catalog                  cached index (packages[] for our firmware)
 *   POST /install?pkg=SLUG         download, verify sha256, stage, run installer
 *   GET  /job                      progress of the last install job
 *   POST /logs                     dump stderr/stdout of the installer
 *
 * Everything is written under /rw_data/store. The HTTP client speaks plain
 * http:// only; https:// is delegated to a helper command so the transport can
 * be swapped without touching the logic (see STORE_FETCH_HELPER).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef STORE_BUILD_ID
#define STORE_BUILD_ID "store-development"
#endif

#ifndef STORE_VERSION
#define STORE_VERSION "0.1.0"
#endif

/* The app itself is published in the catalogue under this slug, which is what
 * makes a self-update just another install with a special reporting order. */
#define SELF_SLUG "marketplace"

#define MAX_PATH 512
#define MAX_BUF 65536
#define DEFAULT_PORT 8092
#define DEFAULT_HUB "http://127.0.0.1:8002"

static char g_rw[MAX_PATH] = "/rw_data/store";
static char g_hub[MAX_PATH] = DEFAULT_HUB;
static char g_token[128];
static char g_install_id[64];
static char g_firmware[96];
static char g_region[16] = "EU";
static char g_gui[48] = "qml_eco3.0";
static int g_allow_install;
static int g_port = DEFAULT_PORT;

/* ------------------------------------------------------------------ helpers */
static void logmsg(const char *fmt, ...)
{
    va_list ap;
    char line[1024];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    line[n] = '\n';
    line[n + 1] = 0;
    fputs("[store] ", stderr);
    fputs(line, stderr);
    fflush(stderr);
}

static char *read_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return NULL;
    n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static int write_file_atomic(const char *path, const char *data)
{
    char tmp[MAX_PATH];
    FILE *f;
    int fd;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    fwrite(data, 1, strlen(data), f);
    fflush(f);
    fd = fileno(f);
    if (fd >= 0)
        fsync(fd);
    fclose(f);
    return rename(tmp, path);
}

static void mkdir_p(const char *path)
{
    char work[MAX_PATH];
    char *p;

    snprintf(work, sizeof(work), "%s", path);
    for (p = work + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(work, 0755);
        *p = '/';
    }
    mkdir(work, 0755);
}

static void json_escape(const char *in, char *out, size_t size)
{
    size_t o = 0;

    for (; *in && o + 7 < size; in++) {
        if (*in == '"' || *in == '\\') {
            out[o++] = '\\';
            out[o++] = *in;
        } else if ((unsigned char)*in < 0x20) {
            o += snprintf(out + o, size - o, "\\u%04x", (unsigned char)*in);
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

/* Extract the string value of a flat "key":"value" pair; returns 0 on success. */
static int json_get_str(const char *json, const char *key, char *out, size_t size)
{
    char needle[128];
    const char *p;
    size_t o = 0;

    snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(json, needle);
    if (!p)
        return -1;
    p += strlen(needle);
    while (*p == ' ')
        p++;
    if (*p != '"')
        return -1;
    p++;
    while (*p && *p != '"' && o + 1 < size) {
        if (*p == '\\' && p[1]) {
            p++;
            out[o++] = (*p == 'n') ? '\n' : *p;
        } else {
            out[o++] = *p;
        }
        p++;
    }
    out[o] = 0;
    return 0;
}

/* Numeric semver-ish compare: returns <0, 0, >0. */
static int version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        long na = strtol(a, (char **)&a, 10);
        long nb = strtol(b, (char **)&b, 10);
        if (na != nb)
            return na < nb ? -1 : 1;
        while (*a == '.' || *a == '-')
            a++;
        while (*b == '.' || *b == '-')
            b++;
    }
    return 0;
}

static long json_get_int(const char *json, const char *key, long fallback)
{
    char needle[128];
    const char *p;

    snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(json, needle);
    if (!p)
        return fallback;
    p += strlen(needle);
    while (*p == ' ')
        p++;
    return strtol(p, NULL, 10);
}

/* ------------------------------------------------------------------ config */
static void config_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s/%s", g_rw, name);
}

/* Firmware/region/gui key. Overridable from /rw_data/store/{firmware,region,gui};
 * defaults match the build these packages are made for. */
static void load_key_file(const char *name, char *out, size_t size)
{
    char path[MAX_PATH], buf[128];

    config_path(path, sizeof(path), name);
    if (!read_file(path, buf, sizeof(buf)))
        return;
    buf[strcspn(buf, "\r\n")] = 0;
    if (buf[0])
        snprintf(out, size, "%s", buf);
}

static void load_state(void)
{
    char path[MAX_PATH], buf[4096];
    char *p;

    config_path(path, sizeof(path), "config.conf");
    if (read_file(path, buf, sizeof(buf))) {
        for (p = strtok(buf, "\n"); p; p = strtok(NULL, "\n")) {
            if (!strncmp(p, "hub=", 4))
                snprintf(g_hub, sizeof(g_hub), "%s", p + 4);
            else if (!strncmp(p, "allow_install=", 14))
                g_allow_install = atoi(p + 14);
            else if (!strncmp(p, "port=", 5))
                g_port = atoi(p + 5);
            else if (!strncmp(p, "firmware=", 9))
                snprintf(g_firmware, sizeof(g_firmware), "%s", p + 9);
            else if (!strncmp(p, "region=", 7))
                snprintf(g_region, sizeof(g_region), "%s", p + 7);
            else if (!strncmp(p, "gui=", 4))
                snprintf(g_gui, sizeof(g_gui), "%s", p + 4);
        }
    }
    config_path(path, sizeof(path), "device.json");
    if (read_file(path, buf, sizeof(buf))) {
        json_get_str(buf, "install_id", g_install_id, sizeof(g_install_id));
        json_get_str(buf, "device_token", g_token, sizeof(g_token));
    }
    if (!g_install_id[0]) {
        /* A unit without an account still needs a stable id; it is persisted on
         * the first save_device() so the hub sees the same unit every boot. */
        unsigned long seed = (unsigned long)time(NULL) ^ (unsigned long)getpid();
        snprintf(g_install_id, sizeof(g_install_id), "00000000-0000-4000-8000-%012lu",
                 (seed * 2654435761ul) % 1000000000000ul);
    }
    load_key_file("firmware", g_firmware, sizeof(g_firmware));
    load_key_file("region", g_region, sizeof(g_region));
    load_key_file("gui", g_gui, sizeof(g_gui));
    if (!g_firmware[0])
        snprintf(g_firmware, sizeof(g_firmware), "NE_E.EUR.S5W_L.001.001.260128");
}

static void save_config(void)
{
    char path[MAX_PATH], body[MAX_PATH * 4];

    snprintf(body, sizeof(body), "hub=%s\nallow_install=%d\nport=%d\nfirmware=%s\nregion=%s\ngui=%s\n",
             g_hub, g_allow_install, g_port, g_firmware, g_region, g_gui);
    config_path(path, sizeof(path), "config.conf");
    mkdir_p(g_rw);
    write_file_atomic(path, body);
}

static void save_device(const char *token, const char *install_id)
{
    char path[MAX_PATH], body[MAX_BUF], esc_id[128], esc_tok[256];

    json_escape(install_id ? install_id : g_install_id, esc_id, sizeof(esc_id));
    json_escape(token ? token : g_token, esc_tok, sizeof(esc_tok));
    snprintf(body, sizeof(body),
             "{\"install_id\":\"%s\",\"device_token\":\"%s\",\"firmware\":\"%s\",\"saved\":%ld}\n",
             esc_id, esc_tok, g_firmware, (long)time(NULL));
    config_path(path, sizeof(path), "device.json");
    mkdir_p(g_rw);
    write_file_atomic(path, body);
}

static void job_write(const char *state, const char *detail, int percent, const char *pkg)
{
    char path[MAX_PATH], body[MAX_BUF], esc[512];

    json_escape(detail ? detail : "", esc, sizeof(esc));
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"package\":\"%s\",\"percent\":%d,\"detail\":\"%s\",\"updated\":%ld}\n",
             state, pkg ? pkg : "", percent, esc, (long)time(NULL));
    config_path(path, sizeof(path), "job.json");
    write_file_atomic(path, body);
    logmsg("job %s %s (%d%%) %s", state, pkg ? pkg : "-", percent, detail ? detail : "");
}

/* ------------------------------------------------------------------- resolver
 * The daemon ships static, so NSS-based gethostbyname() cannot be trusted on
 * the car's glibc. Resolve A records ourselves: /etc/hosts first, then a plain
 * DNS query to the nameserver from /etc/resolv.conf.
 */
static int dns_lookup(const char *name, struct in_addr *out)
{
    char buf[4096], ns[64] = "8.8.8.8";
    char *line, *saveptr = NULL;
    unsigned char query[512], reply[1024];
    struct sockaddr_in addr;
    struct timeval tv = {2, 0};
    int sock, qlen = 0, n;
    size_t i;

    /* /etc/hosts */
    if (read_file("/etc/hosts", buf, sizeof(buf))) {
        for (line = strtok_r(buf, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
            char ip[64], host[256];
            if (line[0] == '#' || sscanf(line, "%63s %255s", ip, host) != 2)
                continue;
            if (strcasecmp(host, name) == 0 && inet_pton(AF_INET, ip, out) == 1)
                return 0;
        }
    }
    if (read_file("/etc/resolv.conf", buf, sizeof(buf))) {
        for (line = strtok_r(buf, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
            char kw[16], value[64];
            if (sscanf(line, "%15s %63s", kw, value) == 2 && !strcmp(kw, "nameserver"))
                snprintf(ns, sizeof(ns), "%s", value);
        }
    }

    memset(query, 0, sizeof(query));
    query[qlen++] = 0x12; /* id */
    query[qlen++] = 0x34;
    query[qlen++] = 0x01; /* RD */
    query[qlen++] = 0x00;
    query[qlen++] = 0x00; query[qlen++] = 0x01; /* QDCOUNT */
    for (i = 0; i < 4; i++)    /* AN/NS/AR zeroed */
        query[qlen++] = 0x00;
    {
        const char *p = name;
        while (*p) {
            const char *dot = strchr(p, '.');
            size_t len = dot ? (size_t)(dot - p) : strlen(p);
            if (len == 0 || len > 63 || qlen + (int)len + 2 >= (int)sizeof(query))
                return -1;
            query[qlen++] = (unsigned char)len;
            memcpy(query + qlen, p, len);
            qlen += (int)len;
            if (!dot)
                break;
            p = dot + 1;
        }
    }
    query[qlen++] = 0x00;      /* root */
    query[qlen++] = 0x00; query[qlen++] = 0x01; /* A */
    query[qlen++] = 0x00; query[qlen++] = 0x01; /* IN */

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    if (inet_pton(AF_INET, ns, &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }
    if (sendto(sock, query, (size_t)qlen, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    n = (int)recv(sock, reply, sizeof(reply), 0);
    close(sock);
    if (n < 12)
        return -1;
    {
        int answers = (reply[6] << 8) | reply[7];
        int qd = (reply[4] << 8) | reply[5];
        int off = 12, a;
        for (a = 0; a < qd; a++) { /* skip question */
            while (off < n && reply[off])
                off += reply[off] + 1;
            off += 5;
        }
        for (a = 0; a < answers && off + 16 <= n; a++) {
            int rdlen;
            if ((reply[off] & 0xC0) == 0xC0) {
                off += 2;
            } else {
                while (off < n && reply[off])
                    off += reply[off] + 1;
                off += 1;
            }
            off += 8; /* type(2) class(2) ttl(4) */
            rdlen = (reply[off] << 8) | reply[off + 1];
            off += 2;
            if (rdlen == 4 && off + 4 <= n && reply[off - 10] == 0 && reply[off - 9] == 1) {
                memcpy(out, reply + off, 4);
                return 0;
            }
            off += rdlen;
        }
    }
    return -1;
}

/* --------------------------------------------------------------- http client */
static int parse_url(const char *url, char *host, size_t host_size, int *port, char *path, size_t path_size)
{
    const char *p = url;
    const char *slash;

    if (!strncmp(p, "http://", 7)) {
        p += 7;
        *port = 80;
    } else if (!strncmp(p, "https://", 8)) {
        return -1; /* plain HTTP only in-process; use STORE_FETCH_HELPER for TLS */
    } else {
        return -1;
    }
    slash = strchr(p, '/');
    if (slash) {
        snprintf(path, path_size, "%s", slash);
    } else {
        snprintf(path, path_size, "/");
    }
    {
        const char *colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));
        if (colon) {
            size_t hlen = (size_t)(colon - p);
            if (hlen >= host_size)
                hlen = host_size - 1;
            memcpy(host, p, hlen);
            host[hlen] = 0;
            *port = atoi(colon + 1);
        } else {
            size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
            if (hlen >= host_size)
                hlen = host_size - 1;
            memcpy(host, p, hlen);
            host[hlen] = 0;
        }
    }
    return 0;
}

/* Fetch into `out` (JSON answers). Returns HTTP status or -1. */
static int http_request(const char *url, const char *method, const char *auth_token,
                        const char *body, char **out, size_t *out_len)
{
    char host[256], path[1024];
    int port = 80, sock;
    struct sockaddr_in addr;
    char request[2048], buffer[MAX_BUF * 4];
    size_t used = 0;
    int header_done = 0;
    char *payload = NULL;
    size_t payload_len = 0;
    long content_length = -1;
    int status = -1;

    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return -2;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 && dns_lookup(host, &addr.sin_addr) != 0) {
        logmsg("cannot resolve %s", host);
        close(sock);
        return -1;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    snprintf(request, sizeof(request),
             "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
             "Accept: application/json\r\n%s%s%s"
             "Content-Length: %zu\r\n\r\n%s",
             method, path, host,
             auth_token ? "Authorization: Device " : "",
             auth_token ? auth_token : "",
             auth_token ? "\r\n" : "",
             body ? strlen(body) : 0,
             body ? body : "");

    if (send(sock, request, strlen(request), 0) < 0) {
        close(sock);
        return -1;
    }

    for (;;) {
        ssize_t n = recv(sock, buffer + used, sizeof(buffer) - used - 1, 0);
        if (n <= 0)
            break;
        used += (size_t)n;
        buffer[used] = 0;
        if (!header_done) {
            char *end = strstr(buffer, "\r\n\r\n");
            if (end) {
                char *cl;
                header_done = 1;
                if (sscanf(buffer, "HTTP/%*d.%*d %d", &status) != 1)
                    status = -1;
                cl = strstr(buffer, "Content-Length:");
                if (!cl)
                    cl = strstr(buffer, "content-length:");
                if (cl)
                    content_length = strtol(cl + 15, NULL, 10);
                payload = end + 4;
            }
        }
        if (header_done && content_length >= 0 && (size_t)(used - (size_t)(payload - buffer)) >= (size_t)content_length) {
            break;
        }
        if (used + 4096 >= sizeof(buffer))
            break;
    }
    close(sock);

    if (!header_done)
        return -1;
    payload_len = used - (size_t)(payload - buffer);
    if (content_length >= 0 && (size_t)content_length < payload_len)
        payload_len = (size_t)content_length;
    *out = malloc(payload_len + 1);
    if (!*out)
        return -1;
    memcpy(*out, payload, payload_len);
    (*out)[payload_len] = 0;
    if (out_len)
        *out_len = payload_len;
    return status;
}

/* Download a URL into a file (artifacts). */
static int http_download(const char *url, const char *dest, const char *auth_token)
{
    char *data = NULL;
    size_t len = 0;
    FILE *f;
    int status = http_request(url, "GET", auth_token, NULL, &data, &len);

    if (status != 200 || !data) {
        free(data);
        return -1;
    }
    f = fopen(dest, "wb");
    if (!f) {
        free(data);
        return -1;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    free(data);
    return 0;
}

/* ------------------------------------------------------------- child helpers */
static int run_capture(const char *cmd, char *out, size_t size)
{
    FILE *p = popen(cmd, "r");
    size_t n;

    if (!p)
        return -1;
    n = fread(out, 1, size - 1, p);
    out[n] = 0;
    return pclose(p);
}

static int sha256_of(const char *path, char *out, size_t size)
{
    char cmd[MAX_PATH * 3], buf[256];
    char *space;

    snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null", path);
    if (run_capture(cmd, buf, sizeof(buf)) != 0 || !buf[0]) {
        snprintf(cmd, sizeof(cmd), "sha256sum.coreutils '%s' 2>/dev/null", path);
        if (run_capture(cmd, buf, sizeof(buf)) != 0 || !buf[0])
            return -1;
    }
    space = strchr(buf, ' ');
    if (space)
        *space = 0;
    snprintf(out, size, "%s", buf);
    return 0;
}

/* --------------------------------------------------------------- state views */
static int find_entry(const char *index, const char *slug, char *out, size_t size);

/* Does the cached index offer a newer Marketplace build than ours? */
static int self_update_available(char *offer_version, size_t size)
{
    char path[MAX_PATH], index[MAX_BUF * 2], entry[MAX_BUF], version[64];

    if (offer_version && size)
        offer_version[0] = 0;
    config_path(path, sizeof(path), "index.json");
    if (!read_file(path, index, sizeof(index)))
        return 0;
    if (find_entry(index, SELF_SLUG, entry, sizeof(entry)) != 0)
        return 0;
    if (json_get_str(entry, "version", version, sizeof(version)) != 0)
        return 0;
    if (offer_version && size)
        snprintf(offer_version, size, "%s", version);
    return version_cmp(version, STORE_VERSION) > 0;
}

static void status_json(char *out, size_t size)
{
    char path[MAX_PATH], job[MAX_BUF], ledger[MAX_BUF], offer[64];

    config_path(path, sizeof(path), "job.json");
    if (!read_file(path, job, sizeof(job)))
        snprintf(job, sizeof(job), "{\"state\":\"idle\",\"percent\":0}");
    config_path(path, sizeof(path), "ledger.json");
    if (!read_file(path, ledger, sizeof(ledger)))
        snprintf(ledger, sizeof(ledger), "{\"installed\":[]}");

    snprintf(out, size,
             "{\"build\":\"%s\",\"version\":\"%s\",\"port\":%d,\"paired\":%s,\"install_id\":\"%s\","
             "\"firmware\":\"%s\",\"region\":\"%s\",\"gui\":\"%s\",\"hub\":\"%s\","
             "\"allow_install\":%s,\"self_update\":%s,\"self_version\":\"%s\","
             "\"job\":%s,\"ledger\":%s}\n",
             STORE_BUILD_ID, STORE_VERSION, g_port, g_token[0] ? "true" : "false", g_install_id,
             g_firmware, g_region, g_gui, g_hub,
             g_allow_install ? "true" : "false",
             self_update_available(offer, sizeof(offer)) ? "true" : "false", offer,
             job, ledger);
}

static void ledger_add(const char *pkg, const char *version, const char *sha)
{
    char path[MAX_PATH], buf[MAX_BUF], esc_pkg[128], esc_ver[64];
    size_t used;

    json_escape(pkg, esc_pkg, sizeof(esc_pkg));
    json_escape(version ? version : "", esc_ver, sizeof(esc_ver));
    config_path(path, sizeof(path), "ledger.json");
    if (!read_file(path, buf, sizeof(buf)))
        snprintf(buf, sizeof(buf), "{\"installed\":[]}");
    used = strlen(buf);
    if (used > 2 && buf[used - 2] == ']') {
        snprintf(buf + used - 2, sizeof(buf) - used + 2,
                 "%s{\"package\":\"%s\",\"version\":\"%s\",\"sha256\":\"%s\",\"installed\":%ld}]}\n",
                 used > 16 ? "," : "", esc_pkg, esc_ver, sha, (long)time(NULL));
        write_file_atomic(path, buf);
    }
}

/* ------------------------------------------------------------------- routes */
static void respond(int fd, int status, const char *body, const char *ctype)
{
    char head[512];
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request"
                         : status == 404 ? "Not Found" : status == 409 ? "Conflict"
                         : status == 502 ? "Bad Gateway" : "Error";
    int n;

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                 status, reason, ctype, strlen(body));
    if (send(fd, head, (size_t)n, 0) < 0)
        return;
    send(fd, body, strlen(body), 0);
}

static void url_param(const char *query, const char *key, char *out, size_t size)
{
    char needle[64];
    const char *p;
    size_t o = 0;

    out[0] = 0;
    snprintf(needle, sizeof(needle), "%s=", key);
    p = strstr(query, needle);
    if (!p)
        return;
    p += strlen(needle);
    while (*p && *p != '&' && o + 1 < size) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else if (*p == '+') {
            out[o++] = ' ';
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

/* POST /pair?code=123456 or ?token=... -> hub, then store the device token. */
static int do_pair(const char *query, char *detail, size_t detail_size)
{
    char code[64], token[128], url[1024], body[512], *resp = NULL;
    char install_id[64], device_token[256], msg[256];
    int status;

    url_param(query, "code", code, sizeof(code));
    url_param(query, "token", token, sizeof(token));
    if (!code[0] && !token[0]) {
        snprintf(detail, detail_size, "code or token required");
        return 400;
    }
    snprintf(body, sizeof(body), code[0] ? "{\"code\":\"%s\"}" : "{\"token\":\"%s\"}",
             code[0] ? code : token);
    snprintf(url, sizeof(url), "%s/api/v1/device/pair", g_hub);
    status = http_request(url, "POST", NULL, body, &resp, NULL);
    if (status == 200 && resp && json_get_str(resp, "install_id", install_id, sizeof(install_id)) == 0 &&
        json_get_str(resp, "device_token", device_token, sizeof(device_token)) == 0) {
        snprintf(g_install_id, sizeof(g_install_id), "%s", install_id);
        snprintf(g_token, sizeof(g_token), "%s", device_token);
        save_device(NULL, NULL);
        snprintf(detail, detail_size, "paired %s", install_id);
        free(resp);
        return 200;
    }
    if (resp && json_get_str(resp, "detail", msg, sizeof(msg)) == 0)
        snprintf(detail, detail_size, "%s", msg);
    else
        snprintf(detail, detail_size, "hub unreachable (%d)", status);
    free(resp);
    return status == 409 ? 409 : 400;
}

/* POST /refresh -> register (anonymous) + fetch the index for our key. */
static int do_refresh(char *detail, size_t detail_size)
{
    char url[1024], body[512], *resp = NULL, *index = NULL;
    char path[MAX_PATH], pair_code[32], pair_url[512], qr_url[512];
    int status;
    size_t len = 0;

    snprintf(body, sizeof(body),
             "{\"install_id\":\"%s\",\"firmware\":\"%s\",\"region\":\"%s\",\"gui\":\"%s\",\"store_version\":\"%s\"}",
             g_install_id, g_firmware, g_region, g_gui, STORE_BUILD_ID);
    snprintf(url, sizeof(url), "%s/api/v1/device/register", g_hub);
    status = http_request(url, "POST", NULL, body, &resp, NULL);
    if (status == 200 && resp) {
        json_get_str(resp, "pairing_code", pair_code, sizeof(pair_code));
        json_get_str(resp, "pair_url", pair_url, sizeof(pair_url));
        json_get_str(resp, "qr_url", qr_url, sizeof(qr_url));
    }
    free(resp);

    snprintf(url, sizeof(url), "%s/api/v1/index/%s/%s/%s", g_hub, g_firmware, g_region, g_gui);
    status = http_request(url, "GET", g_token[0] ? g_token : NULL, NULL, &index, &len);
    if (status != 200 || !index) {
        snprintf(detail, detail_size, "index fetch failed (%d)", status);
        free(index);
        return 502;
    }
    config_path(path, sizeof(path), "index.json");
    write_file_atomic(path, index);
    free(index);
    snprintf(detail, detail_size, "index cached (%zu bytes)", len);
    return 200;
}

/* Find one package entry in the cached index by slug. */
/* Whitespace-tolerant: the hub's JSON may be `"pkg": "demo"` or `"pkg":"demo"`. */
static const char *find_key_value(const char *json, const char *key, const char *value)
{
    char needle[96];
    const char *p = json;

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    while ((p = strstr(p, needle)) != NULL) {
        const char *v = p + strlen(needle);
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v == ':')
            v++;
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v == '"' && !strncmp(v + 1, value, strlen(value)) &&
            v[1 + strlen(value)] == '"')
            return p;
        p += strlen(needle);
    }
    return NULL;
}

static int find_entry(const char *index, const char *slug, char *out, size_t size)
{
    const char *p, *start;
    int depth = 0;
    size_t o = 0;

    p = find_key_value(index, "pkg", slug);
    if (!p)
        return -1;
    start = p;
    while (start > index && *start != '{')
        start--;
    /* Copy the object this entry lives in (brace-counted, strings respected). */
    p = start;
    while (*p && o + 2 < size) {
        if (*p == '"') {
            out[o++] = *p++;
            while (*p && o + 2 < size) {
                out[o++] = *p;
                if (*p == '\\' && p[1] && o + 2 < size) {
                    out[o++] = p[1];
                    p += 2;
                    continue;
                }
                if (*p++ == '"')
                    break;
            }
            continue;
        }
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            out[o++] = *p++;
            if (depth <= 0)
                break;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return o > 2 ? 0 : -1;
}

static void install_worker(const char *slug)
{
    /* The server reaps children automatically (SIGCHLD=SIG_IGN); restore the
     * default here or every system()/popen() in this child returns -1. */
    signal(SIGCHLD, SIG_DFL);
    char entry[MAX_BUF], version[64], url[1024], sha[80], installer_sha[80];
    char index[MAX_BUF * 2], path[MAX_PATH], stage[MAX_PATH], cmd[MAX_PATH * 4];
    char got[80], log_path[MAX_PATH];
    long size;
    FILE *logf;

    config_path(path, sizeof(path), "index.json");
    if (!read_file(path, index, sizeof(index))) {
        job_write("failed", "no cached index - refresh first", 0, slug);
        _exit(1);
    }
    if (find_entry(index, slug, entry, sizeof(entry)) != 0) {
        job_write("failed", "package not in index", 0, slug);
        _exit(1);
    }
    json_get_str(entry, "version", version, sizeof(version));
    json_get_str(entry, "sha256", sha, sizeof(sha));
    json_get_str(entry, "url", url, sizeof(url));
    json_get_str(entry, "installer_sha256", installer_sha, sizeof(installer_sha));
    size = json_get_int(entry, "size", 0);
    if (!url[0] || !sha[0]) {
        job_write("failed", "index entry has no artifact url/sha256", 0, slug);
        _exit(1);
    }

    snprintf(stage, sizeof(stage), "%s/stage/%s", g_rw, slug);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", stage, stage);
    if (system(cmd) != 0) {
        job_write("failed", "cannot create stage dir", 0, slug);
        _exit(1);
    }
    snprintf(path, sizeof(path), "%s/artifact.bin", stage);

    job_write("downloading", url, 5, slug);
    if (http_download(url, path, g_token[0] ? g_token : NULL) != 0) {
        job_write("failed", "download failed", 5, slug);
        _exit(1);
    }
    job_write("verifying", "sha256", 35, slug);
    if (sha256_of(path, got, sizeof(got)) != 0) {
        job_write("failed", "sha256sum unavailable", 35, slug);
        _exit(1);
    }
    if (strcasecmp(got, sha) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "sha256 mismatch: got %s want %s", got, sha);
        job_write("failed", msg, 35, slug);
        unlink(path);
        _exit(1);
    }

    job_write("extracting", "tar", 50, slug);
    snprintf(cmd, sizeof(cmd), "tar -xzf '%s' -C '%s' 2>/dev/null", path, stage);
    if (system(cmd) != 0) {
        job_write("failed", "tar failed", 50, slug);
        _exit(1);
    }
    unlink(path);

    if (!g_allow_install) {
        job_write("staged", "allow_install=0: nothing was executed", 100, slug);
        ledger_add(slug, version, sha);
        _exit(0);
    }
    if (!access(stage, F_OK) || 1) {
        snprintf(cmd, sizeof(cmd), "test -f '%s/run_stuff.sh'", stage);
        if (system(cmd) == 0 && installer_sha[0]) {
            char isha[80];
            snprintf(cmd, sizeof(cmd), "'%s/run_stuff.sh'", stage);
            if (sha256_of(cmd, isha, sizeof(isha)) == 0 && strcasecmp(isha, installer_sha) != 0) {
                job_write("failed", "installer sha256 mismatch", 55, slug);
                _exit(1);
            }
        }
    }

    config_path(log_path, sizeof(log_path), "install.log");
    logf = fopen(log_path, "w");
    job_write("installing", "running run_stuff.sh", 60, slug);
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && USB_DIR='%s' sh ./run_stuff.sh >>'%s' 2>&1", stage, stage, log_path);
    {
        int rc = system(cmd);
        if (logf)
            fclose(logf);
        if (rc != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "installer exited %d (see /logs)", WEXITSTATUS(rc));
            job_write("failed", msg, 90, slug);
            _exit(1);
        }
    }
    ledger_add(slug, version, sha);
    job_write("installed", "run_stuff.sh finished", 100, slug);

    /* Report the result to the hub (best effort). A self-update restarts this
     * service a moment later, so its report is queued on disk first and flushed
     * on the next start (report.pending.json). */
    if (g_token[0]) {
        char url[1024], body[512], *resp = NULL;
        snprintf(body, sizeof(body),
                 "{\"release\":%ld,\"status\":\"ok\",\"artifact_sha256\":\"%s\"}",
                 json_get_int(entry, "release_id", 0), sha);
        if (!strcmp(slug, SELF_SLUG)) {
            config_path(path, sizeof(path), "report.pending.json");
            write_file_atomic(path, body);
        } else {
            snprintf(url, sizeof(url), "%s/api/v1/device/report", g_hub);
            http_request(url, "POST", g_token, body, &resp, NULL);
            free(resp);
        }
    }
    (void)size;
    _exit(0);
}

/* Flush a report that a self-update could not send before the restart. */
static void flush_pending_report(void)
{
    char path[MAX_PATH], body[512], url[1024], *resp = NULL;

    config_path(path, sizeof(path), "report.pending.json");
    if (!read_file(path, body, sizeof(body)))
        return;
    if (!g_token[0])
        return;
    snprintf(url, sizeof(url), "%s/api/v1/device/report", g_hub);
    if (http_request(url, "POST", g_token, body, &resp, NULL) == 200)
        unlink(path);
    else
        logmsg("pending report still unsent (%s)", g_hub);
    free(resp);
}

static void route(int fd, const char *method, const char *target, const char *body)
{
    char path[512], query[1024], out[MAX_BUF * 4], detail[512], slug[64];
    const char *q;
    size_t plen = 0;

    q = strchr(target, '?');
    plen = q ? (size_t)(q - target) : strlen(target);
    if (plen >= sizeof(path))
        plen = sizeof(path) - 1;
    memcpy(path, target, plen);
    path[plen] = 0;
    snprintf(query, sizeof(query), "%s", q ? q + 1 : "");

    if (!strcmp(path, "/status")) {
        status_json(out, sizeof(out));
        respond(fd, 200, out, "application/json");
        return;
    }
    if (!strcmp(path, "/config") && !strcmp(method, "POST")) {
        char hub[512];
        url_param(query, "hub", hub, sizeof(hub));
        if (hub[0])
            snprintf(g_hub, sizeof(g_hub), "%s", hub);
        if (strstr(query, "allow_install="))
            g_allow_install = atoi(strstr(query, "allow_install=") + 14);
        save_config();
        snprintf(out, sizeof(out), "{\"hub\":\"%s\",\"allow_install\":%s}\n", g_hub,
                 g_allow_install ? "true" : "false");
        respond(fd, 200, out, "application/json");
        return;
    }
    if (!strcmp(path, "/pair") && !strcmp(method, "POST")) {
        int status = do_pair(query, detail, sizeof(detail));
        char esc[1024];
        json_escape(detail, esc, sizeof(esc));
        snprintf(out, sizeof(out), "{\"status\":%d,\"detail\":\"%s\"}\n", status, esc);
        respond(fd, status == 200 ? 200 : status, out, "application/json");
        return;
    }
    if (!strcmp(path, "/refresh") && !strcmp(method, "POST")) {
        int status = do_refresh(detail, sizeof(detail));
        char esc[1024];
        json_escape(detail, esc, sizeof(esc));
        snprintf(out, sizeof(out), "{\"status\":%d,\"detail\":\"%s\"}\n", status, esc);
        respond(fd, status, out, "application/json");
        return;
    }
    if (!strcmp(path, "/catalog")) {
        char index[MAX_BUF * 2];
        config_path(out, sizeof(out), "index.json");
        if (!read_file(out, index, sizeof(index))) {
            respond(fd, 404, "{\"detail\":\"no cached index\"}\n", "application/json");
            return;
        }
        respond(fd, 200, index, "application/json");
        return;
    }
    if (!strcmp(path, "/install") && !strcmp(method, "POST")) {
        pid_t pid;
        url_param(query, "pkg", slug, sizeof(slug));
        if (!slug[0]) {
            respond(fd, 400, "{\"detail\":\"pkg required\"}\n", "application/json");
            return;
        }
        job_write("starting", "queued", 1, slug);
        pid = fork();
        if (pid == 0) {
            setsid();
            install_worker(slug);
            _exit(1);
        }
        snprintf(out, sizeof(out), "{\"started\":true,\"package\":\"%s\"}\n", slug);
        respond(fd, 200, out, "application/json");
        return;
    }
    if ((!strcmp(path, "/self-update") || !strcmp(path, "/self_update")) && !strcmp(method, "POST")) {
        char offer[64];
        pid_t pid;

        if (!self_update_available(offer, sizeof(offer))) {
            snprintf(out, sizeof(out),
                     "{\"started\":false,\"detail\":\"already at %s\",\"version\":\"%s\"}\n",
                     STORE_VERSION, STORE_VERSION);
            respond(fd, 200, out, "application/json");
            return;
        }
        job_write("starting", "self-update", 1, SELF_SLUG);
        pid = fork();
        if (pid == 0) {
            setsid();
            install_worker(SELF_SLUG);
            _exit(1);
        }
        snprintf(out, sizeof(out),
                 "{\"started\":true,\"package\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}\n",
                 SELF_SLUG, STORE_VERSION, offer);
        respond(fd, 200, out, "application/json");
        return;
    }
    if (!strcmp(path, "/job")) {
        char job[MAX_BUF];
        config_path(out, sizeof(out), "job.json");
        if (read_file(out, job, sizeof(job)))
            respond(fd, 200, job, "application/json");
        else
            respond(fd, 200, "{\"state\":\"idle\",\"percent\":0}\n", "application/json");
        return;
    }
    if (!strcmp(path, "/logs")) {
        char log[MAX_BUF * 4];
        config_path(out, sizeof(out), "install.log");
        if (read_file(out, log, sizeof(log)))
            respond(fd, 200, log, "text/plain; charset=utf-8");
        else
            respond(fd, 404, "no install log\n", "text/plain; charset=utf-8");
        return;
    }
    if (!strcmp(path, "/health")) {
        respond(fd, 200, "{\"status\":\"ok\"}\n", "application/json");
        return;
    }
    (void)body;
    respond(fd, 404, "{\"detail\":\"unknown endpoint\"}\n", "application/json");
}

/* --------------------------------------------------------------------- main */
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    int listen_fd, one = 1;
    struct sockaddr_in addr;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rw-dir") && i + 1 < argc)
            snprintf(g_rw, sizeof(g_rw), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--hub") && i + 1 < argc)
            snprintf(g_hub, sizeof(g_hub), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--allow-install"))
            g_allow_install = 1;
        else if (!strcmp(argv[i], "--version")) {
            printf("store_daemon %s\n", STORE_BUILD_ID);
            return 0;
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--rw-dir DIR] [--hub URL] [--port N] [--allow-install] "
                   "[--version]\n", argv[0]);
            return 0;
        }
    }

    mkdir_p(g_rw);
    load_state();
    /* A self-update restarts this process; the report it could not send before
     * the restart is queued on disk and flushed here. */
    flush_pending_report();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGCHLD, SIG_IGN);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    listen(listen_fd, 8);
    logmsg("listening on 127.0.0.1:%d hub=%s fw=%s fw_ok=%d allow_install=%d", g_port, g_hub,
           g_firmware, g_firmware[0] ? 1 : 0, g_allow_install);

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        fd_set rfds;
        struct timeval tv = {1, 0};
        int ready;
        int fd;
        char buffer[MAX_BUF];
        ssize_t n;
        char method[16], target[1024], body[MAX_BUF];

        /* select() rather than a blocking accept(): the service is stopped with
         * SIGTERM and must exit promptly instead of waiting for a request. */
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        ready = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0)
            continue;
        fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            /* A client that gave up between select() and accept() must not stop
             * the daemon. */
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
                continue;
            logmsg("accept failed: %s", strerror(errno));
            break;
        }
        n = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            close(fd);
            continue;
        }
        buffer[n] = 0;
        method[0] = target[0] = body[0] = 0;
        if (sscanf(buffer, "%15s %1023s", method, target) != 2) {
            close(fd);
            continue;
        }
        {
            char *hdr = strstr(buffer, "\r\n\r\n");
            if (hdr)
                snprintf(body, sizeof(body), "%s", hdr + 4);
        }
        route(fd, method, target, body);
        close(fd);
    }
    close(listen_fd);
    logmsg("stopped");
    return 0;
}
