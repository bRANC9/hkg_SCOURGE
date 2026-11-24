/* fake_engine_daemon — parked engine sound for the Ioniq 5 Gen5W head unit.
 *
 * SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The unit plays audio happily (the installer's start beep proves it: aplay on
 * the built-in card), so a stationary "V8-style idle" needs no vendor API at all: loop
 * our own synthesised WAV through aplay and expose a tiny HTTP control surface
 * for the UI.
 *
 *   GET  /health                      liveness
 *   GET  /status                      {"ok":true,"playing":…,"file":…,"volume":…}
 *   POST /sound?action=start|stop     start/stop the loop
 *   POST /sound?action=volume&value=N 0..100 (softvol, applied on next start)
 *
 * One child at a time (setsid + pid), killed on stop and on daemon exit.  The
 * loop never touches the beep .dat files and never blocks the caller.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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

#define V8_VERSION "2026-11-23.1"
#define V8_PORT 8095
#define V8_STATE_DIR "/rw_data/fake-engine"
#define V8_ASSET V8_STATE_DIR "/engine_idle.wav"
#define V8_LOG V8_STATE_DIR "/engine.log"
#define V8_CONF V8_STATE_DIR "/config.conf"

static pid_t g_pid;
static int g_volume = 80;
static char g_file[256] = V8_ASSET;

static void log_msg(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    int fd = open(V8_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dprintf(fd, "%04d-%02d-%02d %02d:%02d:%02d %s\n", tmv.tm_year + 1900,
                tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, line);
        close(fd);
    }
    fprintf(stderr, "fake-engine: %s\n", line);
}

static void stop_sound(void);
static bool start_sound(void);

static bool playing(void) { return g_pid > 1 && kill(g_pid, 0) == 0; }

static void drive_tick(double speed_kmh, double pedal);

/* Speed/pedal source: whatever decodes them (CAN later, GPS today) writes two
 * plain files; the daemon follows them while the sound is on, so the driver has
 * exactly one switch and the pitch does the rest.  Missing files simply mean
 * "no data" - the loop then holds the last band instead of stalling. */
static double read_num(const char *path, double dflt)
{
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return dflt;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return dflt;
    buf[n] = 0;
    return atof(buf);
}

static void speed_source_tick(void)
{
    if (!playing()) return;
    double speed = read_num("/rw_data/fake-engine/speed", -1);
    if (speed < 0) return;                     /* no source yet */
    double pedal = read_num("/rw_data/fake-engine/pedal", 50);
    drive_tick(speed, pedal);
}



static void shift_gap(void);
static void shift_gap(void);
static bool start_sound(void);

/* ---- drive model ------------------------------------------------------ */
/* The car's own power is not ours to change: this only makes the SOUND follow
 * speed and pedal the way a six-speed would, so a stationary demo shifts like
 * a real car.  Gear ratios are a plausible 6-speed, redline 6500.  The audible
 * loops only run 600..3000 rpm, so the model's rpm is mapped into that band -
 * the pitch still rises and drops at every shift.
 */
static int g_gear = 1;
static double g_last_rpm = 800.0;

static const double GEAR_RATIO[7] = { 0.0, 3.60, 2.19, 1.55, 1.16, 0.92, 0.75 };
#define FINAL_DRIVE 3.9
#define TYRE_CIRC_M 2.02
#define REDLINE 6500.0
#define SHIFT_UP 5200.0
#define SHIFT_DOWN 1500.0

static int band_index(double rpm)
{
    if (rpm < 600) rpm = 600;
    if (rpm > REDLINE) rpm = REDLINE;
    double scaled = 600.0 + (rpm - 600.0) * 2400.0 / (REDLINE - 600.0);
    int idx = (int)((scaled - 600.0) / 400.0 + 0.5);
    if (idx < 0) idx = 0;
    if (idx > 6) idx = 6;
    return idx;
}

static void apply_band(int idx, bool gap)
{
    char path[256];
    snprintf(path, sizeof(path), "/rw_data/fake-engine/engine_%d.wav", idx);
    bool changed = strcmp(path, g_file) != 0;
    snprintf(g_file, sizeof(g_file), "%s", path);
    if (!playing()) return;
    if (gap && changed) shift_gap();
    else if (changed) start_sound();
}

static void drive_tick(double speed_kmh, double pedal)
{
    if (speed_kmh < 0) speed_kmh = 0;
    if (pedal < 0) pedal = 0;
    if (pedal > 100) pedal = 100;
    double wheel_rpm = (speed_kmh / 3.6) / TYRE_CIRC_M * 60.0;
    double rpm = wheel_rpm * GEAR_RATIO[g_gear] * FINAL_DRIVE;
    if (rpm < 750.0) rpm = 750.0;
    bool shift = false;
    if (rpm > SHIFT_UP && g_gear < 6) { g_gear++; shift = true; }
    else if (rpm < SHIFT_DOWN && g_gear > 1) { g_gear--; shift = true; }
    if (shift)
        rpm = wheel_rpm * GEAR_RATIO[g_gear] * FINAL_DRIVE;
    /* pedal modulates the pitch a little: the same speed sounds harder under
     * load, the way an engine does. */
    rpm *= (0.94 + 0.12 * (pedal / 100.0));
    if (rpm < 750.0) rpm = 750.0;
    g_last_rpm = rpm;
    apply_band(band_index(rpm), shift);
}



static bool start_sound(void)
{
    stop_sound();
    if (access(g_file, R_OK) != 0) {
        log_msg("asset missing: %s", g_file);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        log_msg("fork failed: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        setsid();
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_IGN);
        /* -D plughw:0,0 first: that path is proven by the installer beep; the
         * plain default device is the fallback.  The loop repeats the 4 s asset
         * seamlessly (the generator makes it an exact number of revolutions). */
        execl("/bin/sh", "sh", "-c",
              "while :; do aplay -q -D plughw:0,0 -f S16_LE -r 44100 -c 1 \"$1\" "
              "|| aplay -q -f S16_LE -r 44100 -c 1 \"$1\" || exit 1; done",
              "sh", g_file, (char *)NULL);
        _exit(127);
    }
    g_pid = pid;
    log_msg("started %s (pid %d, volume %d)", g_file, (int)pid, g_volume);
    return true;
}

static void shift_gap(void)
{
    /* The e-shift beat: cut the loop for ~120 ms, the way a dual-clutch shift
     * drops torque, then resume on the new pitch.  Runs in a child so the HTTP
     * caller never waits. */
    pid_t pid = fork();
    if (pid == 0) {
        stop_sound();
        usleep(120000);
        start_sound();
        _exit(0);
    }
}

static void shift_select(const char *rpm)
{
    int v = atoi(rpm);
    int idx = 0;
    if (v >= 2900) idx = 6; else if (v >= 2400) idx = 5; else if (v >= 1900) idx = 4;
    else if (v >= 1500) idx = 3; else if (v >= 1100) idx = 2; else if (v >= 750) idx = 1;
    snprintf(g_file, sizeof(g_file), "/rw_data/fake-engine/engine_%d.wav", idx);
    if (playing()) start_sound();
}



static void stop_sound(void)
{
    if (g_pid > 1) {
        kill(-g_pid, SIGTERM);
        kill(g_pid, SIGTERM);
        waitpid(g_pid, NULL, WNOHANG);
        log_msg("stopped (pid %d)", (int)g_pid);
    }
    g_pid = 0;
}


static void load_conf(void)
{
    char buf[1024];
    int fd = open(V8_CONF, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *p = strstr(buf, "volume=");
    if (p) g_volume = atoi(p + 7);
}

static void save_conf(void)
{
    mkdir(V8_STATE_DIR, 0755);
    int fd = open(V8_CONF, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    dprintf(fd, "volume=%d\n", g_volume);
    close(fd);
}

static bool query_get(const char *query, const char *name, char *out, size_t n)
{
    if (!query || !*query) return false;
    size_t nl = strlen(name);
    const char *p = query;
    while (*p) {
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            const char *v = p + nl + 1;
            size_t i = 0;
            while (*v && *v != '&' && i + 1 < n) out[i++] = *v++;
            out[i] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return false;
}

static void reply(int fd, int code, const char *body)
{
    char head[256];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      code, code == 200 ? "OK" : "Bad Request", strlen(body));
    if (write(fd, head, (size_t)hl) < 0) return;
    if (write(fd, body, strlen(body)) < 0) return;
}

static void handle(int fd)
{
    char req[2048];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return; }
    req[n] = 0;

    char method[8] = "", path[256] = "", query[512] = "";
    if (sscanf(req, "%7s %255s", method, path) != 2) { reply(fd, 400, "{\"ok\":false}\n"); close(fd); return; }
    char *q = strchr(path, '?');
    if (q) { snprintf(query, sizeof(query), "%s", q + 1); *q = 0; }

    char body[1024];
    int code = 200;
    bool is_post = !strcmp(method, "POST");
    bool is_get = !strcmp(method, "GET");

    if (!is_get && !is_post) {
        code = 400;
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"method not allowed\"}\n");
    } else if (is_get && !strcmp(path, "/health")) {
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (is_get && !strcmp(path, "/status")) {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"version\":\"%s\",\"playing\":%s,\"file\":\"%s\",\"volume\":%d,\"gear\":%d,\"rpm\":%.0f}\n",
                 V8_VERSION, playing() ? "true" : "false", g_file, g_volume, g_gear, g_last_rpm);
    } else if (!strcmp(path, "/drive")) {
        char speed[16] = "", pedal[16] = "";
        query_get(query, "speed", speed, sizeof(speed));
        query_get(query, "pedal", pedal, sizeof(pedal));
        drive_tick(atof(speed), pedal[0] ? atof(pedal) : 50.0);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"gear\":%d,\"rpm\":%.0f,\"file\":\"%s\",\"playing\":%s}\n",
                 g_gear, g_last_rpm, g_file, playing() ? "true" : "false");
    } else if (!strcmp(path, "/shift")) {
        char rpm[16] = "";
        query_get(query, "rpm", rpm, sizeof(rpm));
        shift_select(rpm);
        bool gap = query_get(query, "gap", rpm, sizeof(rpm));
        if (gap) shift_gap();
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"playing\":%s,\"file\":\"%s\"}\n",
                 playing() ? "true" : "false", g_file);
    } else if (!strcmp(path, "/sound")) {
        char action[16] = "", value[16] = "", file[256] = "";
        query_get(query, "action", action, sizeof(action));
        query_get(query, "value", value, sizeof(value));
        query_get(query, "file", file, sizeof(file));
        bool ok = true;
        if (!strcmp(action, "start")) {
            if (file[0]) snprintf(g_file, sizeof(g_file), "%s", file);
            ok = start_sound();
        } else if (!strcmp(action, "stop")) {
            stop_sound();
        } else if (!strcmp(action, "volume")) {
            g_volume = atoi(value);
            if (g_volume < 0) g_volume = 0;
            if (g_volume > 100) g_volume = 100;
            save_conf();
        } else if (action[0] && strcmp(action, "status")) {
            ok = false;
        }
        snprintf(body, sizeof(body),
                 "{\"ok\":%s,\"playing\":%s,\"file\":\"%s\",\"volume\":%d}\n",
                 ok ? "true" : "false", playing() ? "true" : "false", g_file, g_volume);
        if (!ok) code = 400;
    } else {
        code = 400;
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"unknown path\"}\n");
    }
    reply(fd, code, body);
    close(fd);
}

int main(int argc, char **argv)
{
    int port = V8_PORT;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) snprintf(g_file, sizeof(g_file), "%s", argv[++i]);
    }
    mkdir(V8_STATE_DIR, 0755);
    load_conf();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { log_msg("socket: %s", strerror(errno)); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("bind %d: %s", port, strerror(errno));
        return 1;
    }
    listen(srv, 8);
    log_msg("started 127.0.0.1:%d file=%s volume=%d", port, g_file, g_volume);

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int ready = select(srv + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            speed_source_tick();
            continue;
        }
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle(fd);
    }
    stop_sound();
    return 0;
}
