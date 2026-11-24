/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * aa_daemon — debug surface for the AA core, for the WiFi page to read.
 *
 * The Android Auto core is not activated yet: there is no transport and no
 * Bluetooth plumbing, so this daemon deliberately does not pretend to run a
 * session.  It exposes the parts that are real today and that are hard to check
 * from a QML page otherwise:
 *
 *   GET /status    service state, build id and whether an AP readback is saved
 *   GET /ap-check  validate an AP readback (ssid, key, bssid, addr, port) with
 *                  the same rules the wireless bootstrap uses, and report what
 *                  would be announced to the phone
 *   GET /saved     the last accepted readback
 *
 * A successful /ap-check is what "save" means here: the validated endpoint is
 * written to the state file, so the AP numbers the WiFi app reports can be
 * pinned while the radio itself is still being stabilised.
 *
 * Usage: aa_daemon [--port N] [--state PATH] [--bind ADDR]
 *        --port 0 binds an ephemeral port and prints "port=<n>" on stdout.
 *
 * The engine under src/ stays free of sockets and stdio; this file is the only
 * one allowed to own a socket, and it is not part of any installer yet.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/aa_ap.h"

#ifndef AA_DAEMON_BUILD_ID
#define AA_DAEMON_BUILD_ID "dev"
#endif

#define AA_DAEMON_DEFAULT_PORT 8095u
#define AA_DAEMON_DEFAULT_STATE "/tmp/aa_daemon_state.conf"
#define AA_DAEMON_REQUEST_MAX 2048u
#define AA_DAEMON_RESPONSE_MAX 2048u

typedef struct {
    bool valid;
    char ssid[AA_BTBOOT_MAX_SSID + 1u];
    char bssid[AA_BTBOOT_BSSID_LEN + 1u];
    char address[AA_AP_MAX_ADDRESS + 1u];
    uint16_t port;
} saved_state;

static volatile sig_atomic_t g_stop;

static void on_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

/* Percent-decode in place; returns false on a malformed escape. */
static bool url_decode(char *text)
{
    size_t out = 0u;
    for (size_t i = 0u; text[i] != '\0'; i++) {
        if (text[i] == '%') {
            if (text[i + 1u] == '\0' || text[i + 2u] == '\0')
                return false;
            int hi = -1;
            int lo = -1;
            const char a = text[i + 1u];
            const char b = text[i + 2u];
            if (a >= '0' && a <= '9')
                hi = a - '0';
            else if (a >= 'a' && a <= 'f')
                hi = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F')
                hi = a - 'A' + 10;
            if (b >= '0' && b <= '9')
                lo = b - '0';
            else if (b >= 'a' && b <= 'f')
                lo = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F')
                lo = b - 'A' + 10;
            if (hi < 0 || lo < 0)
                return false;
            text[out++] = (char)((hi << 4) | lo);
            i += 2u;
            continue;
        }
        text[out++] = text[i];
    }
    text[out] = '\0';
    return true;
}

static bool json_escape(char *out, size_t out_cap, const char *text)
{
    size_t at = 0u;
    if (out == NULL || out_cap == 0u)
        return false;
    for (size_t i = 0u; text != NULL && text[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            if (at + 2u >= out_cap)
                return false;
            out[at++] = '\\';
            out[at++] = (char)c;
            continue;
        }
        if (c < 0x20u) {
            if (at + 6u >= out_cap)
                return false;
            at += (size_t)snprintf(out + at, out_cap - at, "\\u%04x", (unsigned)c);
            continue;
        }
        if (at + 1u >= out_cap)
            return false;
        out[at++] = (char)c;
    }
    out[at] = '\0';
    return true;
}

static bool state_load(const char *path, saved_state *state)
{
    memset(state, 0, sizeof *state);
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return false;
    char line[256];
    while (fgets(line, sizeof line, file) != NULL) {
        char *eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        char *value = eq + 1;
        const size_t len = strlen(value);
        if (len != 0u && value[len - 1u] == '\n')
            value[len - 1u] = '\0';
        if (!strcmp(line, "ssid"))
            snprintf(state->ssid, sizeof state->ssid, "%s", value);
        else if (!strcmp(line, "bssid"))
            snprintf(state->bssid, sizeof state->bssid, "%s", value);
        else if (!strcmp(line, "address"))
            snprintf(state->address, sizeof state->address, "%s", value);
        else if (!strcmp(line, "port"))
            state->port = (uint16_t)strtoul(value, NULL, 10);
        else if (!strcmp(line, "valid"))
            state->valid = !strcmp(value, "1");
    }
    fclose(file);
    return state->valid;
}

static bool state_save(const char *path, const saved_state *state)
{
    FILE *file = fopen(path, "w");
    if (file == NULL)
        return false;
    fprintf(file,
            "valid=1\nssid=%s\nbssid=%s\naddress=%s\nport=%u\n",
            state->ssid, state->bssid, state->address, (unsigned)state->port);
    fclose(file);
    return true;
}

/* Find a query parameter value; writes into out. */
static bool query_get(const char *query, const char *name, char *out, size_t out_cap)
{
    const size_t name_len = strlen(name);
    const char *at = query;
    while (at != NULL && *at != '\0') {
        const char *next = strchr(at, '&');
        const size_t len = next == NULL ? strlen(at) : (size_t)(next - at);
        if (len > name_len && !strncmp(at, name, name_len) && at[name_len] == '=') {
            const size_t value_len = len - name_len - 1u;
            if (value_len + 1u > out_cap)
                return false;
            memcpy(out, at + name_len + 1u, value_len);
            out[value_len] = '\0';
            return url_decode(out);
        }
        at = next == NULL ? NULL : next + 1u;
    }
    return false;
}

static void respond(int fd, int code, const char *reason, const char *body)
{
    char header[192];
    const int header_len = snprintf(header, sizeof header,
                                    "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                                    "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                    code, reason, strlen(body));
    if (header_len > 0)
        (void)write(fd, header, (size_t)header_len);
    (void)write(fd, body, strlen(body));
}

static void handle_status(int fd, const saved_state *state)
{
    char ssid[160];
    char bssid[64];
    char address[64];
    (void)json_escape(ssid, sizeof ssid, state->valid ? state->ssid : "");
    (void)json_escape(bssid, sizeof bssid, state->valid ? state->bssid : "");
    (void)json_escape(address, sizeof address, state->valid ? state->address : "");
    /* The readiness fields mirror what the CarPlay tab shows (iAP2, mDNS,
     * ready).  Android Auto needs its own chain, and on this unit only the last
     * link is missing for a reason the page has to be able to show: the
     * Bluetooth stack is the vendor's, so the AA RFCOMM service and HFP AG
     * profile cannot be registered here.  Nothing is reported as running that
     * is not running. */
    char body[AA_DAEMON_RESPONSE_MAX];
    (void)snprintf(body, sizeof body,
                   "{\"service\":\"aa-daemon\",\"build\":\"%s\",\"state\":\"idle\","
                   "\"transport\":\"none\",\"bt_bootstrap\":false,\"tcp_listener\":false,"
                   "\"tls\":false,\"session\":false,"
                   "\"bt_stack\":\"vendor\",\"saved\":%s,"
                   "\"saved_ssid\":\"%s\",\"saved_bssid\":\"%s\","
                   "\"saved_address\":\"%s\",\"saved_port\":%u,"
                   "\"reason\":\"vendor vbtd only, no BlueZ: AA RFCOMM/HFP cannot be "
                   "registered here; bridge board required\"}\n",
                   AA_DAEMON_BUILD_ID, state->valid ? "true" : "false", ssid, bssid, address,
                   (unsigned)(state->valid ? state->port : 0u));
    respond(fd, 200, "OK", body);
}

static void handle_ap_check(int fd, const char *query, const char *state_path, saved_state *state)
{
    char ssid[192];
    char key[192];
    char bssid[64];
    char address[64];
    char port_text[16];

    if (!query_get(query, "ssid", ssid, sizeof ssid) || !query_get(query, "key", key, sizeof key) ||
        !query_get(query, "bssid", bssid, sizeof bssid) ||
        !query_get(query, "addr", address, sizeof address) ||
        !query_get(query, "port", port_text, sizeof port_text)) {
        respond(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"missing field\"}\n");
        return;
    }

    aa_ap_readback readback;
    memset(&readback, 0, sizeof readback);
    readback.on_air = true;
    readback.ssid = ssid;
    readback.key = key;
    readback.bssid = bssid;
    readback.address = address;
    readback.port = (uint16_t)strtoul(port_text, NULL, 10);

    aa_ap_endpoint endpoint;
    const aa_ap_status status = aa_ap_from_readback(&readback, &endpoint);
    if (status != AA_AP_OK) {
        char body[256];
        (void)snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}\n",
                       aa_ap_status_name(status));
        respond(fd, 200, "OK", body);
        return;
    }

    aa_btboot_start start;
    aa_btboot_info info;
    if (aa_ap_btboot_start(&endpoint, &start) != AA_AP_OK ||
        aa_ap_btboot_info(&endpoint, &info) != AA_AP_OK) {
        respond(fd, 200, "OK", "{\"ok\":false,\"error\":\"announce\"}\n");
        return;
    }

    saved_state next;
    memset(&next, 0, sizeof next);
    next.valid = true;
    snprintf(next.ssid, sizeof next.ssid, "%s", endpoint.ssid);
    snprintf(next.bssid, sizeof next.bssid, "%s", endpoint.bssid);
    snprintf(next.address, sizeof next.address, "%s", endpoint.address);
    next.port = endpoint.port;
    const bool saved = state_save(state_path, &next);
    if (saved)
        *state = next;

    char safe_ip[64];
    (void)json_escape(safe_ip, sizeof safe_ip, start.ip);
    char body[AA_DAEMON_RESPONSE_MAX];
    (void)snprintf(body, sizeof body,
                   "{\"ok\":true,\"saved\":%s,\"announce_ip\":\"%s\",\"announce_port\":%u,"
                   "\"security\":%u,\"ap_type\":%u}\n",
                   saved ? "true" : "false", safe_ip, (unsigned)start.port,
                   (unsigned)info.security_mode, (unsigned)info.ap_type);
    respond(fd, 200, "OK", body);
}

static void handle_saved(int fd, const saved_state *state)
{
    char body[512];
    if (!state->valid) {
        respond(fd, 200, "OK", "{\"saved\":false}\n");
        return;
    }
    char ssid[160];
    char bssid[64];
    char address[64];
    (void)json_escape(ssid, sizeof ssid, state->ssid);
    (void)json_escape(bssid, sizeof bssid, state->bssid);
    (void)json_escape(address, sizeof address, state->address);
    (void)snprintf(body, sizeof body,
                   "{\"saved\":true,\"ssid\":\"%s\",\"bssid\":\"%s\",\"address\":\"%s\","
                   "\"port\":%u}\n",
                   ssid, bssid, address, (unsigned)state->port);
    respond(fd, 200, "OK", body);
}

static void handle_connection(int fd, const char *state_path, saved_state *state)
{
    char request[AA_DAEMON_REQUEST_MAX];
    const ssize_t got = read(fd, request, sizeof request - 1u);
    if (got <= 0) {
        close(fd);
        return;
    }
    request[got] = '\0';

    char *line_end = strstr(request, "\r\n");
    if (line_end != NULL)
        *line_end = '\0';

    char method[8] = {0};
    char target[1024] = {0};
    if (sscanf(request, "%7s %1023s", method, target) != 2) {
        respond(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"request\"}\n");
        close(fd);
        return;
    }
    if (strcmp(method, "GET") != 0) {
        respond(fd, 405, "Method Not Allowed", "{\"ok\":false,\"error\":\"method\"}\n");
        close(fd);
        return;
    }

    char *query = strchr(target, '?');
    if (query != NULL)
        *query++ = '\0';

    if (!strcmp(target, "/status"))
        handle_status(fd, state);
    else if (!strcmp(target, "/ap-check"))
        handle_ap_check(fd, query == NULL ? "" : query, state_path, state);
    else if (!strcmp(target, "/saved"))
        handle_saved(fd, state);
    else
        respond(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"route\"}\n");
    close(fd);
}

int main(int argc, char **argv)
{
    unsigned port = AA_DAEMON_DEFAULT_PORT;
    const char *state_path = AA_DAEMON_DEFAULT_STATE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--state") && i + 1 < argc)
            state_path = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            (void)printf("usage: aa_daemon [--port N] [--state PATH]\n");
            return 0;
        } else {
            (void)fprintf(stderr, "aa_daemon: unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    saved_state state;
    (void)state_load(state_path, &state);

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        (void)fprintf(stderr, "aa_daemon: socket: %s\n", strerror(errno));
        return 1;
    }
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (bind(listener, (struct sockaddr *)&address, sizeof address) < 0) {
        (void)fprintf(stderr, "aa_daemon: bind: %s\n", strerror(errno));
        close(listener);
        return 1;
    }
    if (listen(listener, 8) < 0) {
        (void)fprintf(stderr, "aa_daemon: listen: %s\n", strerror(errno));
        close(listener);
        return 1;
    }

    socklen_t address_len = sizeof address;
    (void)getsockname(listener, (struct sockaddr *)&address, &address_len);
    (void)printf("port=%u\n", (unsigned)ntohs(address.sin_port));
    (void)fflush(stdout);

    while (!g_stop) {
        const int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle_connection(client, state_path, &state);
    }

    close(listener);
    return 0;
}
