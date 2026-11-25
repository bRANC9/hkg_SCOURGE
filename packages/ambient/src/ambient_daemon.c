/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors */
/* SPDX-License-Identifier: AGPL-3.0-only */
/* AMBIENT_PAYLOAD_260128
 *
 * ambient_daemon — the Ambience app for the Hyundai Ioniq 5 Gen5W head unit.
 *
 * The UI (AmbientPage.qml) only draws and calls this daemon over
 * 127.0.0.1:8094.  The daemon renders an ambient-light frame (one RGB value
 * per zone) at a fixed rate from the selected effect, applies the configured
 * transition, optionally modulates it from live audio levels, and pushes the
 * frame to an output backend.
 *
 * Output backends (the daemon never guesses a bus address):
 *   none    render only (tests, or an idle app)
 *   file    append frames to a JSON-lines file (dry-run / bench / tests)
 *   wled    POST /json/state to an aftermarket RGB(W) controller on the LAN
 *   micom   send a raw CAN frame through the vendor micom D-Bus service, built
 *           from an explicit --micom-template (no default: an unknown module
 *           must not be poked by accident).  UNVALIDATED on a car.
 *
 * Audio reactivity reads raw S16LE mono samples from --audio-cmd (a capture
 * process on stdout, e.g. arecord on a loopback/monitor device) or from
 * --audio-file (a FIFO or a growing file, used by the host tests).  No ALSA
 * library is linked: the unit's glibc 2.25 cannot resolve modern NSS/ALSA
 * dependencies from a static binary.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AMBIENT_DAEMON_VERSION "1.0.0"
#define HTTP_PORT 8094
#define ZONES_MAX 4
#define PRESETS_MAX 16
#define AUDIO_RATE_DEFAULT 16000
#define AUDIO_HOP 160
#define AUDIO_PCM_MAX (AUDIO_HOP * 16)
#define LOG_PATH_MAX 512

/* ------------------------------------------------------------------ color */

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static uint8_t clamp_u8(double v)
{
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return (uint8_t)(v + 0.5);
}

static double clamp_d(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static rgb_t rgb_scale(rgb_t c, double k)
{
    rgb_t o;
    o.r = clamp_u8(c.r * k);
    o.g = clamp_u8(c.g * k);
    o.b = clamp_u8(c.b * k);
    return o;
}

static rgb_t rgb_mix(rgb_t a, rgb_t b, double t)
{
    rgb_t o;
    o.r = clamp_u8(a.r + (b.r - a.r) * t);
    o.g = clamp_u8(a.g + (b.g - a.g) * t);
    o.b = clamp_u8(a.b + (b.b - a.b) * t);
    return o;
}

static rgb_t hsv_to_rgb(double h, double s, double v)
{
    h = h - floor(h);
    s = clamp_d(s, 0.0, 1.0);
    v = clamp_d(v, 0.0, 1.0);
    double c = v * s;
    double hp = h * 6.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double m = v - c;
    double r = 0.0, g = 0.0, b = 0.0;
    if (hp < 1.0)      { r = c; g = x; }
    else if (hp < 2.0) { r = x; g = c; }
    else if (hp < 3.0) { g = c; b = x; }
    else if (hp < 4.0) { g = x; b = c; }
    else if (hp < 5.0) { r = x; b = c; }
    else               { r = c; b = x; }
    rgb_t o;
    o.r = clamp_u8((r + m) * 255.0);
    o.g = clamp_u8((g + m) * 255.0);
    o.b = clamp_u8((b + m) * 255.0);
    return o;
}

static void rgb_to_hsv(rgb_t c, double *h, double *s, double *v)
{
    double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn;
    *v = mx;
    *s = mx <= 0.0 ? 0.0 : d / mx;
    if (d <= 0.0) { *h = 0.0; return; }
    double hh;
    if (mx == r)      hh = fmod((g - b) / d, 6.0);
    else if (mx == g) hh = (b - r) / d + 2.0;
    else              hh = (r - g) / d + 4.0;
    hh /= 6.0;
    if (hh < 0.0) hh += 1.0;
    *h = hh;
}

/* -------------------------------------------------------------------- time */

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t wall_s(void)
{
    time_t t = time(NULL);
    return (int64_t)t;
}

/* --------------------------------------------------------------------- log */

static char g_log_path[LOG_PATH_MAX];
static bool g_log_quiet;

static void log_msg(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!g_log_quiet) {
        fprintf(stderr, "ambient_daemon: %s\n", line);
        fflush(stderr);
    }
    if (!g_log_path[0]) return;
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    fprintf(f, "%lld %s\n", (long long)wall_s(), line);
    fclose(f);
}

static void log_cap(void)
{
    if (!g_log_path[0]) return;
    struct stat st;
    if (stat(g_log_path, &st) != 0) return;
    if (st.st_size <= 1024 * 1024) return;
    FILE *in = fopen(g_log_path, "r");
    if (!in) return;
    if (fseek(in, st.st_size - 512 * 1024, SEEK_SET) != 0) { fclose(in); return; }
    char keep[512 * 1024 + 1];
    size_t n = fread(keep, 1, sizeof(keep) - 1, in);
    fclose(in);
    keep[n] = 0;
    FILE *out = fopen(g_log_path, "w");
    if (!out) return;
    fputs(keep, out);
    fclose(out);
}

static void write_atomic(const char *path, const char *text)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        log_msg("cannot write %s: %s", path, strerror(errno));
        return;
    }
    fputs(text, f);
    fflush(f);
    fclose(f);
    if (rename(tmp, path) != 0)
        log_msg("cannot rename %s: %s", path, strerror(errno));
}

static int read_file(const char *path, char *out, size_t n)
{
    if (!n) return -1;
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t got = fread(out, 1, n - 1, f);
    fclose(f);
    out[got] = 0;
    return (int)got;
}

/* ------------------------------------------------------------------- state */

enum {
    FX_OFF = 0, FX_SOLID, FX_BREATHE, FX_PULSE, FX_RAINBOW,
    FX_WAVE, FX_COMET, FX_SPARKLE, FX_CYCLE, FX_COUNT
};

static const char *const FX_NAMES[FX_COUNT] = {
    "off", "solid", "breathe", "pulse", "rainbow",
    "wave", "comet", "sparkle", "cycle"
};

static const char *const FX_DESC[FX_COUNT] = {
    "Lights off",
    "One steady colour",
    "Slow brightness breathing",
    "Beat-synced flash (music)",
    "Hue rotation, one offset per zone",
    "Soft travelling brightness wave",
    "Travelling head with a fading tail",
    "Random short twinkles",
    "Drifts through hues around the base colour"
};

enum { TR_CROSSFADE = 0, TR_BLACK, TR_CUT };
static const char *const TR_NAMES[3] = { "crossfade", "black", "cut" };

enum { BK_NONE = 0, BK_FILE, BK_WLED, BK_MICOM };
static const char *const BK_NAMES[4] = { "none", "file", "wled", "micom" };

typedef struct {
    bool power;
    int effect;
    rgb_t color;
    int brightness;
    int speed;
    int intensity;
    int transition_ms;
    int transition_style;
    bool reactive;
    int audio_gain;
    int zones;
} amb_state;

static amb_state g_state;

static char g_state_dir[256] = "/rw_data/ambient";
static char g_config_path[400];
static char g_status_path[400];
static char g_presets_path[400];

static int g_backend = BK_NONE;
static char g_target[256];
static int g_target_port = 80;
static int g_fps = 25;
static int g_output_hz = 20;
static char g_micom_send[256];
static char g_micom_template[128];
static bool g_persist = true;
static bool g_stop;

static void state_defaults(void)
{
    g_state.power = true;
    g_state.effect = FX_BREATHE;
    g_state.color.r = 0x2b;
    g_state.color.g = 0x6c;
    g_state.color.b = 0xff;
    g_state.brightness = 70;
    g_state.speed = 45;
    g_state.intensity = 50;
    g_state.transition_ms = 800;
    g_state.transition_style = TR_CROSSFADE;
    g_state.reactive = false;
    g_state.audio_gain = 110;
    g_state.zones = ZONES_MAX;
}

static void state_clamp(void)
{
    if (g_state.effect < 0 || g_state.effect >= FX_COUNT) g_state.effect = FX_SOLID;
    g_state.brightness = (int)clamp_d(g_state.brightness, 0, 100);
    g_state.speed = (int)clamp_d(g_state.speed, 0, 100);
    g_state.intensity = (int)clamp_d(g_state.intensity, 0, 100);
    g_state.transition_ms = (int)clamp_d(g_state.transition_ms, 0, 10000);
    if (g_state.transition_style < 0 || g_state.transition_style > TR_CUT)
        g_state.transition_style = TR_CROSSFADE;
    g_state.audio_gain = (int)clamp_d(g_state.audio_gain, 0, 400);
    g_state.zones = (int)clamp_d(g_state.zones, 1, ZONES_MAX);
}

static int effect_by_name(const char *name)
{
    for (int i = 0; i < FX_COUNT; i++)
        if (!strcasecmp(name, FX_NAMES[i])) return i;
    return -1;
}

static int transition_by_name(const char *name)
{
    for (int i = 0; i < 3; i++)
        if (!strcasecmp(name, TR_NAMES[i])) return i;
    return -1;
}

static int backend_by_name(const char *name)
{
    for (int i = 0; i < 4; i++)
        if (!strcasecmp(name, BK_NAMES[i])) return i;
    return -1;
}

static const char *backend_name(void)
{
    return BK_NAMES[g_backend];
}

static const char *effect_name(void)
{
    return FX_NAMES[g_state.effect];
}

/* "RRGGBB", "#RRGGBB" or "r,g,b". */
static bool parse_color(const char *in, rgb_t *out)
{
    while (*in == ' ') in++;
    if (*in == '#') in++;
    if (strchr(in, ',')) {
        int r = 0, g = 0, b = 0;
        if (sscanf(in, "%d,%d,%d", &r, &g, &b) != 3) return false;
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return false;
        out->r = (uint8_t)r; out->g = (uint8_t)g; out->b = (uint8_t)b;
        return true;
    }
    if (strlen(in) != 6) return false;
    unsigned int v = 0;
    for (const char *p = in; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else return false;
        v = v * 16 + (unsigned)d;
    }
    out->r = (uint8_t)((v >> 16) & 0xff);
    out->g = (uint8_t)((v >> 8) & 0xff);
    out->b = (uint8_t)(v & 0xff);
    return true;
}

static void config_load(void)
{
    char text[4096];
    if (read_file(g_config_path, text, sizeof(text)) < 0) return;
    char *line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char *key = line;
            char *val = eq + 1;
            if (!strcmp(key, "power")) g_state.power = atoi(val) != 0;
            else if (!strcmp(key, "effect")) {
                int fx = effect_by_name(val);
                if (fx >= 0) g_state.effect = fx;
            } else if (!strcmp(key, "color")) {
                rgb_t c;
                if (parse_color(val, &c)) g_state.color = c;
            } else if (!strcmp(key, "brightness")) g_state.brightness = atoi(val);
            else if (!strcmp(key, "speed")) g_state.speed = atoi(val);
            else if (!strcmp(key, "intensity")) g_state.intensity = atoi(val);
            else if (!strcmp(key, "transition_ms")) g_state.transition_ms = atoi(val);
            else if (!strcmp(key, "transition_style")) {
                int tr = transition_by_name(val);
                if (tr >= 0) g_state.transition_style = tr;
            } else if (!strcmp(key, "reactive")) g_state.reactive = atoi(val) != 0;
            else if (!strcmp(key, "audio_gain")) g_state.audio_gain = atoi(val);
            else if (!strcmp(key, "zones")) g_state.zones = atoi(val);
            else if (!strcmp(key, "backend")) {
                int bk = backend_by_name(val);
                if (bk >= 0) g_backend = bk;
            } else if (!strcmp(key, "target")) {
                snprintf(g_target, sizeof(g_target), "%s", val);
            } else if (!strcmp(key, "target_port")) g_target_port = atoi(val);
            else if (!strcmp(key, "fps")) g_fps = atoi(val);
            else if (!strcmp(key, "output_hz")) g_output_hz = atoi(val);
            else if (!strcmp(key, "micom_template")) {
                snprintf(g_micom_template, sizeof(g_micom_template), "%s", val);
            } else if (!strcmp(key, "micom_send")) {
                snprintf(g_micom_send, sizeof(g_micom_send), "%s", val);
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    state_clamp();
}

static void config_save(void)
{
    if (!g_persist) return;
    char text[2048];
    snprintf(text, sizeof(text),
             "power=%d\n"
             "effect=%s\n"
             "color=%02x%02x%02x\n"
             "brightness=%d\n"
             "speed=%d\n"
             "intensity=%d\n"
             "transition_ms=%d\n"
             "transition_style=%s\n"
             "reactive=%d\n"
             "audio_gain=%d\n"
             "zones=%d\n"
             "backend=%s\n"
             "target=%s\n"
             "target_port=%d\n"
             "fps=%d\n"
             "output_hz=%d\n"
             "micom_template=%s\n"
             "micom_send=%s\n",
             g_state.power ? 1 : 0, effect_name(),
             g_state.color.r, g_state.color.g, g_state.color.b,
             g_state.brightness, g_state.speed, g_state.intensity,
             g_state.transition_ms, TR_NAMES[g_state.transition_style],
             g_state.reactive ? 1 : 0, g_state.audio_gain, g_state.zones,
             backend_name(), g_target, g_target_port, g_fps, g_output_hz,
             g_micom_template, g_micom_send);
    write_atomic(g_config_path, text);
}

/* ---------------------------------------------------------------- presets */

typedef struct {
    char name[32];
    amb_state st;
} preset_t;

static preset_t g_presets[PRESETS_MAX];
static int g_preset_count;

static void preset_add(const char *name, amb_state st)
{
    if (g_preset_count >= PRESETS_MAX) return;
    for (int i = 0; i < g_preset_count; i++)
        if (!strcasecmp(g_presets[i].name, name)) { g_presets[i].st = st; return; }
    snprintf(g_presets[g_preset_count].name, sizeof(g_presets[0].name), "%s", name);
    g_presets[g_preset_count].st = st;
    g_preset_count++;
}

static int preset_index(const char *name)
{
    for (int i = 0; i < g_preset_count; i++)
        if (!strcasecmp(g_presets[i].name, name)) return i;
    return -1;
}

static void presets_save(void)
{
    if (!g_persist) return;
    char text[4096];
    size_t off = 0;
    for (int i = 0; i < g_preset_count; i++) {
        amb_state *s = &g_presets[i].st;
        int n = snprintf(text + off, sizeof(text) - off,
                         "%s|%s|%d,%d,%d|%d|%d|%d|%d|%s|%d\n",
                         g_presets[i].name, FX_NAMES[s->effect],
                         s->color.r, s->color.g, s->color.b,
                         s->brightness, s->speed, s->intensity,
                         s->transition_ms, TR_NAMES[s->transition_style],
                         s->reactive ? 1 : 0);
        if (n <= 0 || (size_t)n >= sizeof(text) - off) break;
        off += (size_t)n;
    }
    write_atomic(g_presets_path, text);
}

static void presets_defaults(void)
{
    amb_state s;
    memset(&s, 0, sizeof(s));

    s.power = true; s.effect = FX_BREATHE;
    s.color.r = 0x2b; s.color.g = 0x6c; s.color.b = 0xff;
    s.brightness = 40; s.speed = 30; s.intensity = 40;
    s.transition_ms = 1200; s.transition_style = TR_CROSSFADE;
    s.reactive = false; s.audio_gain = 110; s.zones = ZONES_MAX;
    preset_add("Relax", s);

    s.effect = FX_WAVE; s.speed = 50; s.brightness = 70;
    s.color.r = 0x00; s.color.g = 0xd1; s.color.b = 0xff;
    s.transition_ms = 600; s.transition_style = TR_CROSSFADE;
    preset_add("Drive", s);

    s.effect = FX_RAINBOW; s.speed = 80; s.brightness = 85;
    s.intensity = 65; s.reactive = true; s.audio_gain = 130;
    s.transition_style = TR_CROSSFADE; s.transition_ms = 400;
    preset_add("Party", s);

    s.effect = FX_SOLID; s.brightness = 18; s.reactive = false;
    s.speed = 25; s.intensity = 40;
    s.color.r = 0x8a; s.color.g = 0x3b; s.color.b = 0x12;
    s.transition_style = TR_BLACK; s.transition_ms = 900;
    preset_add("Night", s);
}

static void presets_load(void)
{
    presets_defaults();
    char text[4096];
    if (read_file(g_presets_path, text, sizeof(text)) > 0) {
        char *line = text;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char *f[10];
            int nf = 0;
            char *p = line;
            while (nf < 10) {
                f[nf++] = p;
                char *bar = strchr(p, '|');
                if (!bar) break;
                *bar = 0;
                p = bar + 1;
            }
            if (nf >= 9) {
                amb_state s;
                memset(&s, 0, sizeof(s));
                s.power = true;
                int fx = effect_by_name(f[1]);
                s.effect = fx >= 0 ? fx : FX_SOLID;
                rgb_t c;
                if (!parse_color(f[2], &c)) c = g_state.color;
                s.color = c;
                s.brightness = atoi(f[3]);
                s.speed = atoi(f[4]);
                s.intensity = atoi(f[5]);
                s.transition_ms = atoi(f[6]);
                int tr = transition_by_name(f[7]);
                s.transition_style = tr >= 0 ? tr : TR_CROSSFADE;
                s.reactive = atoi(f[8]) != 0;
                s.audio_gain = 110;
                s.zones = ZONES_MAX;
                size_t len = strlen(f[0]);
                if (len > 0 && len < sizeof(g_presets[0].name))
                    preset_add(f[0], s);
            }
            if (!nl) break;
            line = nl + 1;
        }
    }
}

/* ------------------------------------------------------------------ audio */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

static void biquad_init(biquad_t *b, float rate, float freq, float q)
{
    float w0 = 2.0f * (float)M_PI * freq / rate;
    float alpha = sinf(w0) / (2.0f * q);
    float cw = cosf(w0);
    float a0 = 1.0f + alpha;
    b->b0 = alpha / a0;
    b->b1 = 0.0f;
    b->b2 = -alpha / a0;
    b->a1 = (-2.0f * cw) / a0;
    b->a2 = (1.0f - alpha) / a0;
    b->z1 = 0.0f;
    b->z2 = 0.0f;
}

static float biquad_run(biquad_t *b, float in)
{
    float out = b->b0 * in + b->z1;
    b->z1 = b->b1 * in - b->a1 * out + b->z2;
    b->z2 = b->b2 * in - b->a2 * out;
    return out;
}

typedef struct {
    bool enabled;
    char source[96];
    int fd;
    pid_t pid;
    char error[128];
    float rate;
    int16_t pcm[AUDIO_PCM_MAX];
    size_t pcm_len;
    unsigned char raw[4096];
    biquad_t bass, mid, high;
    float env, level, bass_env, mid_env, high_env;
    float bass_avg, mid_avg, high_avg;
    float flash;
    int beats;
    int64_t clock_ms;
    int64_t last_beat_ms;
    int64_t open_retry_ms;
} audio_t;

static audio_t g_audio;
static char g_audio_cmd[256];
static char g_audio_file[256];
static float g_audio_rate = AUDIO_RATE_DEFAULT;

static void audio_open(void)
{
    if (g_audio.fd >= 0) return;
    int64_t now = mono_ms();
    if (now < g_audio.open_retry_ms) return;
    g_audio.open_retry_ms = now + 2000;

    if (g_audio_file[0]) {
        int fd = open(g_audio_file, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            snprintf(g_audio.error, sizeof(g_audio.error), "open %s: %s",
                     g_audio_file, strerror(errno));
            return;
        }
        g_audio.fd = fd;
        g_audio.enabled = true;
        snprintf(g_audio.source, sizeof(g_audio.source), "file:%s", g_audio_file);
        log_msg("audio source: %s", g_audio.source);
        return;
    }
    if (!g_audio_cmd[0]) return;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(g_audio.error, sizeof(g_audio.error), "pipe: %s", strerror(errno));
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(g_audio.error, sizeof(g_audio.error), "fork: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", g_audio_cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    g_audio.fd = pipefd[0];
    g_audio.pid = pid;
    g_audio.enabled = true;
    snprintf(g_audio.source, sizeof(g_audio.source), "cmd:%s", g_audio_cmd);
    log_msg("audio source: %s (pid %d)", g_audio.source, (int)pid);
}

static float env_follow(float env, float in, float attack, float release)
{
    if (in > env) return env + (in - env) * attack;
    return env + (in - env) * release;
}

static void audio_hop(void)
{
    float sum = 0.0f, bsum = 0.0f, msum = 0.0f, hsum = 0.0f;
    for (int i = 0; i < AUDIO_HOP; i++) {
        float x = (float)g_audio.pcm[i] / 32768.0f;
        sum += x * x;
        float b = biquad_run(&g_audio.bass, x);
        float m = biquad_run(&g_audio.mid, x);
        float h = biquad_run(&g_audio.high, x);
        bsum += b * b;
        msum += m * m;
        hsum += h * h;
    }
    float rms = sqrtf(sum / (float)AUDIO_HOP);
    float be = (float)clamp_d((double)(sqrtf(bsum / (float)AUDIO_HOP) * 1.8f), 0.0, 1.0);
    float me = (float)clamp_d((double)(sqrtf(msum / (float)AUDIO_HOP) * 1.8f), 0.0, 1.0);
    float he = (float)clamp_d((double)(sqrtf(hsum / (float)AUDIO_HOP) * 1.8f), 0.0, 1.0);

    g_audio.env = env_follow(g_audio.env, rms, 0.55f, 0.08f);
    g_audio.bass_env = env_follow(g_audio.bass_env, be, 0.6f, 0.06f);
    g_audio.mid_env = env_follow(g_audio.mid_env, me, 0.5f, 0.08f);
    g_audio.high_env = env_follow(g_audio.high_env, he, 0.5f, 0.08f);

    float gain = (float)g_state.audio_gain / 100.0f;
    g_audio.level = (float)clamp_d((double)(g_audio.env * gain * 1.4f), 0.0, 1.0);

    g_audio.bass_avg += (g_audio.bass_env - g_audio.bass_avg) * 0.02f;
    g_audio.mid_avg += (g_audio.mid_env - g_audio.mid_avg) * 0.02f;
    g_audio.high_avg += (g_audio.high_env - g_audio.high_avg) * 0.02f;

    /* Beat timing runs on the audio clock: a buffered capture delivers
     * several hops in one loop tick, and wall-clock spacing would swallow
     * every beat but the first. */
    g_audio.clock_ms += (int64_t)(1000.0 * (double)AUDIO_HOP / (double)g_audio.rate);
    if (g_audio.bass_env > g_audio.bass_avg * 1.45f + 0.01f &&
        g_audio.bass_env > 0.05f &&
        g_audio.clock_ms - g_audio.last_beat_ms > 150) {
        g_audio.beats++;
        g_audio.last_beat_ms = g_audio.clock_ms;
        g_audio.flash = 1.0f;
    }
    g_audio.flash *= 0.86f;
}

static void audio_tick(void)
{
    if (g_audio.fd < 0 && (g_audio_cmd[0] || g_audio_file[0])) audio_open();
    if (!g_audio.enabled || g_audio.fd < 0) return;

    for (;;) {
        ssize_t n = read(g_audio.fd, g_audio.raw, sizeof(g_audio.raw));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            snprintf(g_audio.error, sizeof(g_audio.error), "read: %s", strerror(errno));
            close(g_audio.fd);
            g_audio.fd = -1;
            g_audio.enabled = false;
            break;
        }
        if (n == 0) break;
        size_t have = (size_t)n / 2;
        for (size_t i = 0; i < have; i++) {
            int16_t s = (int16_t)((uint16_t)g_audio.raw[i * 2] |
                                  ((uint16_t)g_audio.raw[i * 2 + 1] << 8));
            if (g_audio.pcm_len < AUDIO_PCM_MAX)
                g_audio.pcm[g_audio.pcm_len++] = s;
        }
        if (g_audio.pcm_len < AUDIO_HOP) continue;
        while (g_audio.pcm_len >= AUDIO_HOP) {
            audio_hop();
            memmove(g_audio.pcm, g_audio.pcm + AUDIO_HOP,
                    (g_audio.pcm_len - AUDIO_HOP) * sizeof(g_audio.pcm[0]));
            g_audio.pcm_len -= AUDIO_HOP;
        }
    }
}

/* ----------------------------------------------------------------- engine */

static rgb_t g_last_frame[ZONES_MAX];
static rgb_t g_from_frame[ZONES_MAX];
static int64_t g_trans_start;
static bool g_trans_active;
static int64_t g_phase_ms;
static unsigned long g_frames;

static double rand01(unsigned a, unsigned b)
{
    unsigned h = a * 2654435761u ^ b * 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return (double)(h & 0xffffffu) / (double)0x1000000u;
}

static void effect_frame(double t, rgb_t *out)
{
    double sp = 0.25 + 2.0 * (double)g_state.speed / 100.0;
    double hue0, sat0, val0;
    rgb_to_hsv(g_state.color, &hue0, &sat0, &val0);
    int zones = g_state.zones > 0 ? g_state.zones : 1;

    for (int z = 0; z < ZONES_MAX; z++) {
        out[z].r = out[z].g = out[z].b = 0;
        if (z >= zones) continue;
        switch (g_state.effect) {
        case FX_OFF:
            break;
        case FX_SOLID:
            out[z] = g_state.color;
            break;
        case FX_BREATHE: {
            double k = 0.25 + 0.75 * (0.5 + 0.5 * sin(t * sp * 1.5708));
            out[z] = rgb_scale(g_state.color, k);
            break;
        }
        case FX_PULSE: {
            double k;
            if (g_audio.enabled && g_audio.level > 0.001) {
                k = 0.18 + 0.82 * clamp_d(0.65 * g_audio.level + 0.5 * g_audio.flash, 0.0, 1.0);
            } else {
                k = 0.25 + 0.75 * (0.5 + 0.5 * sin(t * sp * 2.0 * M_PI / 1.5));
            }
            out[z] = rgb_scale(g_state.color, k);
            break;
        }
        case FX_RAINBOW: {
            double hue = fmod(t * sp * 0.12 + (double)z / (double)zones, 1.0);
            out[z] = hsv_to_rgb(hue, 1.0, 1.0);
            break;
        }
        case FX_WAVE: {
            double v = 0.3 + 0.7 * (0.5 + 0.5 * sin(t * sp * 1.2 - (double)z * 1.1));
            out[z] = hsv_to_rgb(hue0, sat0, v);
            break;
        }
        case FX_COMET: {
            double head = fmod(t * sp * 0.6, 1.0) * (double)zones;
            double d = fabs(head - (double)z);
            if (d > (double)zones / 2.0) d = (double)zones - d;
            double k = clamp_d(1.0 - d / 1.8, 0.0, 1.0);
            k *= k;
            if (g_audio.enabled) k = clamp_d(k + 0.35 * g_audio.flash, 0.0, 1.0);
            out[z] = rgb_scale(g_state.color, k);
            break;
        }
        case FX_SPARKLE: {
            out[z] = rgb_scale(g_state.color, 0.22);
            double p = 0.10 * (double)g_state.intensity / 50.0;
            if (rand01((unsigned)(t * 8.0), (unsigned)z) < p)
                out[z] = rgb_scale(g_state.color, 0.85 + 0.15 * g_audio.flash);
            break;
        }
        case FX_CYCLE: {
            double hue = hue0 + fmod(t * sp * 0.08, 1.0);
            out[z] = hsv_to_rgb(hue, sat0, 1.0);
            break;
        }
        default:
            break;
        }
    }

    if (g_state.reactive && g_audio.enabled) {
        double e = g_audio.level;
        double shift = 0.10 * (double)(g_audio.high_env - g_audio.mid_env);
        for (int z = 0; z < ZONES_MAX; z++) {
            if (z >= zones) continue;
            double h, s, v;
            rgb_to_hsv(out[z], &h, &s, &v);
            h = fmod(h + shift + 1.0, 1.0);
            v = clamp_d(v * (0.35 + 1.45 * e), 0.0, 1.0);
            out[z] = hsv_to_rgb(h, s, v);
        }
    }

    double bri = (double)g_state.brightness / 100.0;
    for (int z = 0; z < ZONES_MAX; z++)
        out[z] = rgb_scale(out[z], bri);
}

static double ease_io(double t)
{
    t = clamp_d(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

static void render_frame(int64_t now, rgb_t *out)
{
    double t = (double)(now - g_phase_ms) / 1000.0;
    if (!g_state.power) {
        for (int z = 0; z < ZONES_MAX; z++) {
            out[z].r = out[z].g = out[z].b = 0;
        }
    } else {
        effect_frame(t, out);
    }

    if (g_trans_active) {
        double p = g_state.transition_ms > 0
                   ? (double)(now - g_trans_start) / (double)g_state.transition_ms
                   : 1.0;
        p = clamp_d(p, 0.0, 1.0);
        int style = g_state.transition_style;
        for (int z = 0; z < ZONES_MAX; z++) {
            if (style == TR_CUT) break;
            if (style == TR_BLACK) {
                double k = p < 0.5 ? 1.0 - 2.0 * p : 2.0 * p - 1.0;
                out[z] = rgb_scale(out[z], k < 0.0 ? 0.0 : k);
            } else {
                out[z] = rgb_mix(g_from_frame[z], out[z], ease_io(p));
            }
        }
        if (p >= 1.0) g_trans_active = false;
    }

    for (int z = 0; z < ZONES_MAX; z++) g_last_frame[z] = out[z];
    g_frames++;
}

static void state_changed(void)
{
    int64_t now = mono_ms();
    for (int z = 0; z < ZONES_MAX; z++) g_from_frame[z] = g_last_frame[z];
    g_phase_ms = now;
    g_trans_active = g_state.transition_ms > 0 &&
                     g_state.transition_style != TR_CUT;
    g_trans_start = now;
}

/* ----------------------------------------------------------------- output */

static int64_t g_last_out_ms;
static rgb_t g_last_sent[ZONES_MAX];
static bool g_last_sent_valid;
static unsigned long g_frames_sent;
static unsigned long g_output_errors;
static char g_last_error[320];

static int resolve_ipv4(const char *host, struct in_addr *out)
{
    if (inet_pton(AF_INET, host, out) == 1) return 0;
    /* A static glibc cannot use NSS; /etc/hosts is what the unit has. */
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return -1;
    char line[512];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char ip[64], name[256], alias[256];
        int n = sscanf(line, "%63s %255s %255s", ip, name, alias);
        if (n >= 2 && (!strcmp(name, host) || (n == 3 && !strcmp(alias, host))) &&
            inet_pton(AF_INET, ip, out) == 1) {
            rc = 0;
            break;
        }
    }
    fclose(f);
    return rc;
}

static int tcp_connect(const char *host, int port, int timeout_ms)
{
    struct in_addr addr;
    if (resolve_ipv4(host, &addr) != 0) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "cannot resolve %s (use an IPv4 address or /etc/hosts)", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "socket: %s", strerror(errno));
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr = addr;
    sa.sin_port = htons((uint16_t)port);

    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            snprintf(g_last_error, sizeof(g_last_error), "connect %s:%d timed out", host, port);
            close(fd);
            return -1;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
            snprintf(g_last_error, sizeof(g_last_error), "connect %s:%d: %s", host, port,
                     strerror(soerr ? soerr : errno));
            close(fd);
            return -1;
        }
    } else if (rc < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "connect %s:%d: %s", host, port,
                 strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int http_post_json(const char *host, int port, const char *body)
{
    int fd = tcp_connect(host, port, 300);
    if (fd < 0) return -1;

    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "POST /json/state HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     host, strlen(body), body);
    if (n <= 0) { close(fd); return -1; }
    size_t left = (size_t)n;
    const char *p = req;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) {
            if (errno == EAGAIN) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                if (poll(&pfd, 1, 300) > 0) continue;
            }
            snprintf(g_last_error, sizeof(g_last_error), "write: %s", strerror(errno));
            close(fd);
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    char buf[256];
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)read(fd, buf, sizeof(buf));
    close(fd);
    g_last_error[0] = 0;
    return 0;
}

static void output_file(const rgb_t *frame, int zones)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "{\"t\":%lld,\"z\":[",
                     (long long)mono_ms());
    for (int z = 0; z < zones && n > 0 && (size_t)n < sizeof(line); z++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s[%u,%u,%u]",
                      z ? "," : "", frame[z].r, frame[z].g, frame[z].b);
    }
    snprintf(line + (n > 0 ? n : 0), sizeof(line) - (size_t)(n > 0 ? n : 0), "]}\n");

    const char *path = g_target[0] ? g_target : "frames.jsonl";
    FILE *f = fopen(path, "a");
    if (!f) {
        snprintf(g_last_error, sizeof(g_last_error), "open %s: %s", path, strerror(errno));
        g_output_errors++;
        return;
    }
    fputs(line, f);
    fclose(f);
    g_last_error[0] = 0;
}

static void output_wled(const rgb_t *frame, int zones)
{
    if (!g_target[0]) {
        snprintf(g_last_error, sizeof(g_last_error), "wled backend needs a target");
        g_output_errors++;
        return;
    }
    char body[640];
    int bri = g_state.brightness * 255 / 100;
    int n = snprintf(body, sizeof(body), "{\"on\":%s,\"bri\":%d,\"seg\":[",
                     g_state.power ? "true" : "false", bri);
    for (int z = 0; z < zones && n > 0 && (size_t)n < sizeof(body); z++) {
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "%s{\"id\":%d,\"col\":[[%u,%u,%u]]}",
                      z ? "," : "", z, frame[z].r, frame[z].g, frame[z].b);
    }
    if (n > 0 && (size_t)n < sizeof(body))
        snprintf(body + n, sizeof(body) - (size_t)n, "]}");
    if (http_post_json(g_target, g_target_port, body) != 0) {
        g_output_errors++;
        return;
    }
    g_last_error[0] = 0;
}

static void output_micom(const rgb_t *frame)
{
    if (!g_micom_template[0] || !g_micom_send[0]) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "micom backend needs micom_send and micom_template");
        g_output_errors++;
        return;
    }
    char hex[256];
    size_t off = 0;
    for (const char *p = g_micom_template; *p && off + 8 < sizeof(hex); p++) {
        if (*p == '%' && (p[1] == 'R' || p[1] == 'r')) {
            off += (size_t)snprintf(hex + off, sizeof(hex) - off, "0x%02x,", frame[0].r);
            p++;
        } else if (*p == '%' && (p[1] == 'G' || p[1] == 'g')) {
            off += (size_t)snprintf(hex + off, sizeof(hex) - off, "0x%02x,", frame[0].g);
            p++;
        } else if (*p == '%' && (p[1] == 'B' || p[1] == 'b')) {
            off += (size_t)snprintf(hex + off, sizeof(hex) - off, "0x%02x,", frame[0].b);
            p++;
        } else if (isxdigit((unsigned char)*p)) {
            char pair[3] = { *p, p[1], 0 };
            if (!isxdigit((unsigned char)p[1])) {
                snprintf(g_last_error, sizeof(g_last_error), "bad micom template");
                g_output_errors++;
                return;
            }
            off += (size_t)snprintf(hex + off, sizeof(hex) - off, "0x%02x,",
                                    (unsigned)strtoul(pair, NULL, 16));
            p++;
        }
    }
    if (off && hex[off - 1] == ',') hex[off - 1] = 0;

    char arg[320];
    snprintf(arg, sizeof(arg), "array:byte:%s", hex);

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "fork: %s", strerror(errno));
        g_output_errors++;
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execl(g_micom_send, g_micom_send, arg, (char *)NULL);
        _exit(127);
    }
    g_last_error[0] = 0;
}

static void tick_output(int64_t now, const rgb_t *frame)
{
    if (g_backend == BK_NONE) return;
    if (g_output_hz <= 0) return;
    int64_t interval = 1000 / (g_output_hz > 1000 ? 1000 : g_output_hz);
    if (now - g_last_out_ms < interval) return;
    if (g_last_sent_valid && !memcmp(frame, g_last_sent, sizeof(rgb_t) * ZONES_MAX))
        return;
    g_last_out_ms = now;
    memcpy(g_last_sent, frame, sizeof(rgb_t) * ZONES_MAX);
    g_last_sent_valid = true;
    g_frames_sent++;

    int zones = g_state.zones > 0 ? g_state.zones : 1;
    switch (g_backend) {
    case BK_FILE: output_file(frame, zones); break;
    case BK_WLED: output_wled(frame, zones); break;
    case BK_MICOM: output_micom(frame); break;
    default: break;
    }
}

/* --------------------------------------------------------- session monitor */

static char g_session_bus_file[256] = "/tmp/session_appmgr";
static char g_dbus_monitor[256] = "dbus-monitor";
static pid_t g_mon_pid = -1;
static int g_mon_fd = -1;
static int g_mon_retry_ms;
static int g_home_seq;
static char g_home_source[32] = "none";
static char g_mon_line[512];
static size_t g_mon_line_len;
static bool g_mon_enabled = true;

static void monitor_start(void)
{
    if (!g_mon_enabled || g_mon_fd >= 0) return;
    int64_t now = mono_ms();
    if (now < g_mon_retry_ms) return;
    g_mon_retry_ms = (int)(now + 5000);

    char addr[256];
    if (read_file(g_session_bus_file, addr, sizeof(addr)) < 0) return;
    size_t len = strlen(addr);
    while (len && (addr[len - 1] == '\n' || addr[len - 1] == '\r' || addr[len - 1] == ' '))
        addr[--len] = 0;
    if (!len) return;

    int pipefd[2];
    if (pipe(pipefd) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl(g_dbus_monitor, "dbus-monitor", "--address", addr,
              "type='method_call',member='NotifyRequestHomeScreen'",
              "type='signal',member='custom_key_value'", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    g_mon_fd = pipefd[0];
    g_mon_pid = pid;
    snprintf(g_home_source, sizeof(g_home_source), "dbus");
    log_msg("session monitor started (pid %d)", (int)pid);
}

static void monitor_line(void)
{
    if (strstr(g_mon_line, "NotifyRequestHomeScreen")) {
        g_home_seq++;
        snprintf(g_home_source, sizeof(g_home_source), "home");
    } else if (strstr(g_mon_line, "custom_key_value")) {
        g_home_seq++;
        snprintf(g_home_source, sizeof(g_home_source), "custom");
    }
    g_mon_line_len = 0;
}

static void monitor_tick(void)
{
    if (!g_mon_enabled) return;
    monitor_start();
    if (g_mon_fd < 0) return;
    char buf[512];
    for (;;) {
        ssize_t n = read(g_mon_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close(g_mon_fd);
            g_mon_fd = -1;
            break;
        }
        if (n == 0) {
            close(g_mon_fd);
            g_mon_fd = -1;
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                g_mon_line[g_mon_line_len] = 0;
                monitor_line();
            } else if (g_mon_line_len + 1 < sizeof(g_mon_line)) {
                g_mon_line[g_mon_line_len++] = buf[i];
            }
        }
    }
}

static void reap_children(void)
{
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        if (pid == g_mon_pid) {
            g_mon_pid = -1;
            if (g_mon_fd >= 0) {
                close(g_mon_fd);
                g_mon_fd = -1;
            }
        }
        if (pid == g_audio.pid) {
            g_audio.pid = -1;
            if (g_audio.fd >= 0) {
                close(g_audio.fd);
                g_audio.fd = -1;
            }
            g_audio.enabled = false;
        }
    }
}

/* ------------------------------------------------------------------- json */

static void json_escape(char *out, size_t n, const char *in)
{
    size_t o = 0;
    if (!n) return;
    for (const char *p = in; *p && o + 2 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            int w = snprintf(out + o, n - o, "\\u%04x", c);
            if (w <= 0) break;
            o += (size_t)w;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

static void zone_json(char *out, size_t n, const rgb_t *frame)
{
    size_t o = 0;
    int zones = g_state.zones > 0 ? g_state.zones : 1;
    for (int z = 0; z < zones; z++) {
        int w = snprintf(out + o, n - o, "%s[%u,%u,%u]", z ? "," : "",
                         frame[z].r, frame[z].g, frame[z].b);
        if (w <= 0 || (size_t)w >= n - o) break;
        o += (size_t)w;
    }
}

/* ------------------------------------------------------------------ */
/* On-demand factory-ambient capture                                  */
/*                                                                    */
/* The vendor light is not published on the micom D-Bus SIGNALS this  */
/* unit emits (2026-11-25: only ihu speed/ETC telemetry and radio      */
/* station updates in a 15 s toggle window), and a light COMMAND would */
/* be a method call.  So the capture watches signals and method calls, */
/* on the system and the session bus, and keeps everything that        */
/* mentions com.lge.car.micom.                                        */
/*                                                                    */
/* The filter used to be interface='com.lge.car.micom.can' - an        */
/* interface that does not exist on this unit, so the file it wrote    */
/* was empty and the button looked broken.  Filter by service name     */
/* instead and count what arrived, so the page can say whether the     */
/* round found anything.                                              */
/* ------------------------------------------------------------------ */
static int64_t g_capture_started_ms;

static const char HW_CAPTURE_SH[] =
    "# args: <dbus-monitor> <out> [baseline-s] [window-s]\n"
    "M=\"$1\"; O=\"$2\"; B=8; W=15\n"
    "[ -n \"$3\" ] && B=\"$3\"\n"
    "[ -n \"$4\" ] && W=\"$4\"\n"
    "{\n"
    "  echo \"=== ambient capture (on-demand) $(date -u 2>/dev/null)\"\n"
    "  echo \"--- phase 1: idle baseline (\"$B\"s)\"\n"
    "  \"$M\" --system \"type='signal'\" > /tmp/amb_p1.txt 2>&1 & p1=$!\n"
    "  sleep \"$B\"\n"
    "  kill $p1 2>/dev/null; wait $p1 2>/dev/null\n"
    "  echo \"  phase1: $(grep -c 'member=' /tmp/amb_p1.txt 2>/dev/null) signals, $(grep -c 'com[.]lge[.]car[.]micom' /tmp/amb_p1.txt 2>/dev/null) micom\"\n"
    "  grep -E -A3 'interface=com[.]lge[.]car[.]micom' /tmp/amb_p1.txt 2>/dev/null | head -n 80\n"
    "  echo \"--- phase 2: TOGGLE A LIGHT NOW (\"$W\"s)\"\n"
    "  \"$M\" --system \"type='signal'\" > /tmp/amb_p2.txt 2>&1 & p2=$!\n"
    "  \"$M\" --system \"type='method_call'\" > /tmp/amb_p2c.txt 2>&1 & p3=$!\n"
    "  \"$M\" --session \"type='method_call'\" > /tmp/amb_p2s.txt 2>&1 & p4=$!\n"
    "  sleep \"$W\"\n"
    "  kill $p2 $p3 $p4 2>/dev/null; wait 2>/dev/null\n"
    "  echo \"  phase2: $(grep -c 'member=' /tmp/amb_p2.txt 2>/dev/null) signals, $(grep -c 'member=' /tmp/amb_p2c.txt 2>/dev/null) system calls, $(grep -c 'member=' /tmp/amb_p2s.txt 2>/dev/null) session calls\"\n"
    "  grep -E -A3 'interface=com[.]lge[.]car[.]micom' /tmp/amb_p2.txt 2>/dev/null | head -n 120\n"
    "  echo \"  --- system method calls mentioning light/ambient/micom\"\n"
    "  grep -iE 'light|ambient|illum|theme|mood|micom' /tmp/amb_p2c.txt 2>/dev/null | head -n 60\n"
    "  echo \"  --- session method calls mentioning light/ambient/theme\"\n"
    "  grep -iE 'light|ambient|illum|theme|mood' /tmp/amb_p2s.txt 2>/dev/null | head -n 40\n"
    "  echo \"  --- distinct system members seen\"\n"
    "  grep -oE 'member=[A-Za-z0-9_]+' /tmp/amb_p2c.txt 2>/dev/null | sort | uniq -c | sort -rn | head -n 20\n"
    "  echo \"  --- light-ish hits per phase-2 stream\"\n"
    "  grep -ciE 'light|ambient|illum|theme|mood' /tmp/amb_p2.txt /tmp/amb_p2c.txt /tmp/amb_p2s.txt 2>/dev/null\n"
    "  rm -f /tmp/amb_p1.txt /tmp/amb_p2.txt /tmp/amb_p2c.txt /tmp/amb_p2s.txt\n"
    "} > \"$O\" 2>&1\n";

/* Count what the last capture file holds, so /status can report it (and the page
 * can show whether the round found anything at all). */
static void capture_summary(int *events, int *light_hits)
{
    *events = 0;
    *light_hits = 0;
    char path[320];
    snprintf(path, sizeof(path), "%s/hw_capture.txt", g_state_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n++ < 20000) {
        if (strstr(line, "member=") || strstr(line, "method_call") || strstr(line, "signal ")) (*events)++;
        if (strcasestr(line, "light") || strcasestr(line, "ambient") ||
            strcasestr(line, "illum") || strcasestr(line, "theme") || strcasestr(line, "mood"))
            (*light_hits)++;
    }
    fclose(f);
}

static void capture_state(char *out, size_t n)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/hw_capture.txt", g_state_dir);
    struct stat st;
    if (!g_capture_started_ms) { snprintf(out, n, "idle"); return; }
    if (stat(path, &st) != 0) { snprintf(out, n, "running"); return; }
    if ((int64_t)st.st_mtime * 1000 >= g_capture_started_ms) { snprintf(out, n, "done"); return; }
    snprintf(out, n, "running");
}

static void build_status(char *out, size_t n)
{
    double h, s, v;
    rgb_to_hsv(g_state.color, &h, &s, &v);

    char zones_json[128];
    zone_json(zones_json, sizeof(zones_json), g_last_frame);

    char preset_names[512];
    size_t off = 0;
    preset_names[0] = 0;
    for (int i = 0; i < g_preset_count && off + 40 < sizeof(preset_names); i++) {
        char esc[64];
        json_escape(esc, sizeof(esc), g_presets[i].name);
        off += (size_t)snprintf(preset_names + off, sizeof(preset_names) - off,
                                "%s\"%s\"", i ? "," : "", esc);
    }

    char introspect[256];
    json_escape(introspect, sizeof(introspect), g_home_source);

    char cap_state[16];
    int cap_events = 0, cap_light = 0;
    capture_state(cap_state, sizeof(cap_state));
    capture_summary(&cap_events, &cap_light);

    snprintf(out, n,
             "{\"ok\":true,\"version\":\"%s\",\"power\":%s,\"effect\":\"%s\","
             "\"effect_id\":%d,\"color\":[%u,%u,%u],"
             "\"color_hsv\":[%.3f,%.3f,%.3f],\"brightness\":%d,\"speed\":%d,"
             "\"intensity\":%d,\"transition_ms\":%d,\"transition_style\":\"%s\","
             "\"reactive\":%s,\"audio_gain\":%d,\"zones\":%d,"
             "\"backend\":\"%s\",\"target\":\"%s\",\"target_port\":%d,"
             "\"fps\":%d,\"output_hz\":%d,\"frames\":%lu,\"frames_sent\":%lu,"
             "\"output_errors\":%lu,\"last_error\":\"%s\","
             "\"audio\":{\"enabled\":%s,\"source\":\"%s\",\"level\":%.3f,"
             "\"bass\":%.3f,\"mid\":%.3f,\"high\":%.3f,\"beats\":%d,"
             "\"beat_age_ms\":%lld,\"error\":\"%s\"},"
             "\"zone_colors\":[%s],\"presets\":[%s],"
             "\"home_seq\":%d,\"home_source\":\"%s\",\"home_monitor\":%s,"
             "\"capture\":\"%s\",\"capture_events\":%d,\"capture_light_hits\":%d,"
             "\"config\":\"%s\"}\n",
             AMBIENT_DAEMON_VERSION, g_state.power ? "true" : "false",
             effect_name(), g_state.effect,
             g_state.color.r, g_state.color.g, g_state.color.b,
             h, s, v, g_state.brightness, g_state.speed, g_state.intensity,
             g_state.transition_ms, TR_NAMES[g_state.transition_style],
             g_state.reactive ? "true" : "false", g_state.audio_gain,
             g_state.zones, backend_name(), g_target, g_target_port,
             g_fps, g_output_hz, g_frames, g_frames_sent, g_output_errors,
             g_last_error,
             (g_audio.enabled) ? "true" : "false",
             g_audio.source[0] ? g_audio.source : (g_audio_cmd[0] ? "cmd" :
                                                  (g_audio_file[0] ? "file" : "none")),
             (double)g_audio.level, (double)g_audio.bass_env,
             (double)g_audio.mid_env, (double)g_audio.high_env,
             g_audio.beats,
             g_audio.last_beat_ms ? (long long)(g_audio.clock_ms - g_audio.last_beat_ms) : -1,
             g_audio.error, zones_json, preset_names,
             g_home_seq, introspect, g_mon_enabled ? "true" : "false",
             cap_state, cap_events, cap_light, g_config_path);
}

static void write_status_file(void)
{
    if (!g_status_path[0]) return;
    static char body[8192];
    build_status(body, sizeof(body));
    write_atomic(g_status_path, body);
}

/* ------------------------------------------------------------------- http */

static void http_reply(int fd, int code, const char *body)
{
    const char *reason = code == 200 ? "OK" :
                         code == 202 ? "Accepted" :
                         code == 400 ? "Bad Request" :
                         code == 409 ? "Conflict" : "Error";
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     code, reason, strlen(body));
    if (n > 0) (void)write(fd, hdr, (size_t)n);
    (void)write(fd, body, strlen(body));
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) &&
            isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else if (*r == '+') {
            *w++ = ' ';
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static bool query_get(const char *query, const char *name, char *out, size_t n)
{
    if (out && n) out[0] = 0;
    if (!query) return false;
    size_t nlen = strlen(name);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (eq && (size_t)(eq - p) == nlen && !strncmp(p, name, nlen)) {
            size_t vlen = len - (size_t)(eq - p) - 1;
            char tmp[512];
            if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
            memcpy(tmp, eq + 1, vlen);
            tmp[vlen] = 0;
            url_decode(tmp);
            size_t outlen = strlen(tmp);
            if (outlen >= n) outlen = n - 1;
            memcpy(out, tmp, outlen);
            out[outlen] = 0;
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static bool query_int(const char *query, const char *name, int lo, int hi,
                      int *out, bool *found)
{
    char tmp[64];
    if (found) *found = false;
    if (!query_get(query, name, tmp, sizeof(tmp))) return true;
    char *end = NULL;
    long v = strtol(tmp, &end, 10);
    if (!end || *end || end == tmp) return false;
    if (v < lo || v > hi) return false;
    *out = (int)v;
    if (found) *found = true;
    return true;
}

static void apply_preset(const preset_t *p)
{
    g_state = p->st;
    state_clamp();
}

static void handle_effect(const char *query, char *body, size_t n, int *code)
{
    char name[64];
    if (!query_get(query, "name", name, sizeof(name))) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"name required\"}\n");
        *code = 400;
        return;
    }
    int fx = effect_by_name(name);
    if (fx < 0) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"unknown effect\",\"name\":\"%s\"}\n", name);
        *code = 400;
        return;
    }
    int speed = g_state.speed, intensity = g_state.intensity;
    bool found;
    if (!query_int(query, "speed", 0, 100, &speed, &found)) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"speed must be 0..100\"}\n");
        *code = 400;
        return;
    }
    if (!query_int(query, "intensity", 0, 100, &intensity, &found)) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"intensity must be 0..100\"}\n");
        *code = 400;
        return;
    }
    g_state.effect = fx;
    g_state.speed = speed;
    g_state.intensity = intensity;
    g_state.power = true;
    state_changed();
    config_save();
    snprintf(body, n, "{\"ok\":true,\"effect\":\"%s\",\"speed\":%d,\"intensity\":%d}\n",
             effect_name(), g_state.speed, g_state.intensity);
}

static void handle_color(const char *query, char *body, size_t n, int *code)
{
    char r[32], g[32], b[32], h[32], s[32], v[32];
    bool has_rgb = query_get(query, "r", r, sizeof(r));
    if (has_rgb) {
        if (!query_get(query, "g", g, sizeof(g)) || !query_get(query, "b", b, sizeof(b))) {
            snprintf(body, n, "{\"ok\":false,\"error\":\"r, g and b are required together\"}\n");
            *code = 400;
            return;
        }
        char joined[128];
        snprintf(joined, sizeof(joined), "%s,%s,%s", r, g, b);
        rgb_t c;
        if (!parse_color(joined, &c)) {
            snprintf(body, n, "{\"ok\":false,\"error\":\"colour components must be 0..255\"}\n");
            *code = 400;
            return;
        }
        g_state.color = c;
    } else if (query_get(query, "h", h, sizeof(h))) {
        double hh = atof(h);
        double ss = query_get(query, "s", s, sizeof(s)) ? atof(s) : 1.0;
        double vv = query_get(query, "v", v, sizeof(v)) ? atof(v) : 1.0;
        if (hh < 0.0 || hh > 1.0 || ss < 0.0 || ss > 1.0 || vv < 0.0 || vv > 1.0) {
            snprintf(body, n, "{\"ok\":false,\"error\":\"h, s and v must be 0..1\"}\n");
            *code = 400;
            return;
        }
        g_state.color = hsv_to_rgb(hh, ss, vv);
    } else if (query_get(query, "hex", h, sizeof(h))) {
        rgb_t c;
        if (!parse_color(h, &c)) {
            snprintf(body, n, "{\"ok\":false,\"error\":\"bad hex colour\"}\n");
            *code = 400;
            return;
        }
        g_state.color = c;
    } else {
        snprintf(body, n, "{\"ok\":false,\"error\":\"give r,g,b or h,s,v or hex\"}\n");
        *code = 400;
        return;
    }
    state_changed();
    config_save();
    snprintf(body, n, "{\"ok\":true,\"color\":[%u,%u,%u]}\n",
             g_state.color.r, g_state.color.g, g_state.color.b);
}

static void handle_preset(const char *query, char *body, size_t n, int *code)
{
    char name[64];
    char action[16] = "apply";
    if (!query_get(query, "name", name, sizeof(name))) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"name required\"}\n");
        *code = 400;
        return;
    }
    char action_arg[16];
    if (query_get(query, "action", action_arg, sizeof(action_arg)))
        snprintf(action, sizeof(action), "%s", action_arg);

    if (!strcmp(action, "save")) {
        if (strlen(name) >= sizeof(g_presets[0].name)) {
            snprintf(body, n, "{\"ok\":false,\"error\":\"name too long\"}\n");
            *code = 400;
            return;
        }
        preset_add(name, g_state);
        presets_save();
        snprintf(body, n, "{\"ok\":true,\"saved\":\"%s\",\"presets\":%d}\n",
                 name, g_preset_count);
        return;
    }
    if (!strcmp(action, "delete")) {
        int idx = preset_index(name);
        if (idx < 0) {
            snprintf(body, n, "{\"ok\":false,\"error\":\"no such preset\"}\n");
            *code = 400;
            return;
        }
        for (int i = idx; i + 1 < g_preset_count; i++) g_presets[i] = g_presets[i + 1];
        g_preset_count--;
        presets_save();
        snprintf(body, n, "{\"ok\":true,\"deleted\":\"%s\"}\n", name);
        return;
    }
    if (strcmp(action, "apply")) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"unknown action\"}\n");
        *code = 400;
        return;
    }
    int idx = preset_index(name);
    if (idx < 0) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"no such preset\"}\n");
        *code = 400;
        return;
    }
    apply_preset(&g_presets[idx]);
    state_changed();
    config_save();
    snprintf(body, n, "{\"ok\":true,\"preset\":\"%s\",\"effect\":\"%s\"}\n",
             g_presets[idx].name, effect_name());
}

static void handle_config(const char *query, char *body, size_t n, int *code)
{
    char val[256];
    bool changed = false;

    if (query_get(query, "backend", val, sizeof(val))) {
        int bk = backend_by_name(val);
        if (bk < 0) {
            snprintf(body, n, "{\"ok\":false,\"error\":\"unknown backend\"}\n");
            *code = 400;
            return;
        }
        if (bk == BK_WLED && !g_target[0] && !query_get(query, "target", val, sizeof(val))) {
            snprintf(body, n, "{\"ok\":false,\"error\":\"wled needs a target\"}\n");
            *code = 400;
            return;
        }
        g_backend = bk;
        changed = true;
    }
    if (query_get(query, "target", val, sizeof(val))) {
        snprintf(g_target, sizeof(g_target), "%s", val);
        changed = true;
    }
    bool found = false;
    int num = g_target_port;
    if (!query_int(query, "target_port", 1, 65535, &num, &found)) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"target_port must be 1..65535\"}\n");
        *code = 400;
        return;
    }
    if (found) { g_target_port = num; changed = true; }
    num = g_fps;
    if (!query_int(query, "fps", 1, 120, &num, &found)) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"fps must be 1..120\"}\n");
        *code = 400;
        return;
    }
    if (found) { g_fps = num; changed = true; }
    num = g_output_hz;
    if (!query_int(query, "output_hz", 1, 120, &num, &found)) {
        snprintf(body, n, "{\"ok\":false,\"error\":\"output_hz must be 1..120\"}\n");
        *code = 400;
        return;
    }
    if (found) { g_output_hz = num; changed = true; }
    if (query_get(query, "audio_cmd", val, sizeof(val))) {
        snprintf(g_audio_cmd, sizeof(g_audio_cmd), "%s", val);
        g_audio_file[0] = 0;
        changed = true;
    }
    if (query_get(query, "audio_file", val, sizeof(val))) {
        snprintf(g_audio_file, sizeof(g_audio_file), "%s", val);
        g_audio_cmd[0] = 0;
        changed = true;
    }
    if (query_get(query, "micom_template", val, sizeof(val))) {
        snprintf(g_micom_template, sizeof(g_micom_template), "%.127s", val);
        changed = true;
    }
    if (changed) config_save();
    snprintf(body, n,
             "{\"ok\":true,\"backend\":\"%s\",\"target\":\"%s\",\"target_port\":%d,"
             "\"fps\":%d,\"output_hz\":%d}\n",
             backend_name(), g_target, g_target_port, g_fps, g_output_hz);
}

static void http_handle(int fd)
{
    char req[2048];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return; }
    req[n] = 0;

    char method[8] = { 0 };
    char path[128] = { 0 };
    if (sscanf(req, "%7s %127s", method, path) != 2) {
        http_reply(fd, 400, "{\"ok\":false,\"error\":\"bad request\"}\n");
        close(fd);
        return;
    }

    char *query = NULL;
    char *q = strchr(path, '?');
    if (q) {
        *q = 0;
        query = q + 1;
    }

    static char body[33000];
    body[0] = 0;
    int code = 200;
    bool is_post = !strcmp(method, "POST");
    bool is_get = !strcmp(method, "GET");

    if (!is_get && !is_post) {
        code = 400;
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"method not allowed\"}\n");
    } else if (is_get && !strcmp(path, "/health")) {
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (is_get && !strcmp(path, "/status")) {
        build_status(body, sizeof(body));
    } else if (is_get && !strcmp(path, "/frame")) {
        rgb_t frame[ZONES_MAX];
        render_frame(mono_ms(), frame);
        char zones_json[128];
        zone_json(zones_json, sizeof(zones_json), frame);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"t\":%lld,\"power\":%s,\"effect\":\"%s\","
                 "\"brightness\":%d,\"transition\":%s,\"zones\":[%s]}\n",
                 (long long)mono_ms(), g_state.power ? "true" : "false",
                 effect_name(), g_state.brightness,
                 g_trans_active ? "true" : "false", zones_json);
    } else if (is_get && !strcmp(path, "/effects")) {
        size_t off = 0;
        body[0] = 0;
        for (int i = 0; i < FX_COUNT && off + 128 < sizeof(body); i++) {
            off += (size_t)snprintf(body + off, sizeof(body) - off,
                                    "%s{\"name\":\"%s\",\"description\":\"%s\"}",
                                    i ? "," : "", FX_NAMES[i], FX_DESC[i]);
        }
        char wrapped[8192];
        snprintf(wrapped, sizeof(wrapped), "{\"ok\":true,\"effects\":[%s]}\n", body);
        snprintf(body, sizeof(body), "%s", wrapped);
    } else if (is_get && !strcmp(path, "/presets")) {
        size_t off = 0;
        body[0] = 0;
        for (int i = 0; i < g_preset_count && off + 96 < sizeof(body); i++) {
            amb_state *s = &g_presets[i].st;
            char esc[64];
            json_escape(esc, sizeof(esc), g_presets[i].name);
            off += (size_t)snprintf(body + off, sizeof(body) - off,
                                    "%s{\"name\":\"%s\",\"effect\":\"%s\","
                                    "\"color\":[%u,%u,%u],\"brightness\":%d,"
                                    "\"speed\":%d,\"reactive\":%s}",
                                    i ? "," : "", esc, FX_NAMES[s->effect],
                                    s->color.r, s->color.g, s->color.b,
                                    s->brightness, s->speed,
                                    s->reactive ? "true" : "false");
        }
        char wrapped[8192];
        snprintf(wrapped, sizeof(wrapped), "{\"ok\":true,\"presets\":[%s]}\n", body);
        snprintf(body, sizeof(body), "%s", wrapped);
    } else if (is_get && !strcmp(path, "/logs")) {
        static char logbuf[16385];
        static char logesc[32769];
        if (read_file(g_log_path, logbuf, sizeof(logbuf)) < 0) logbuf[0] = 0;
        json_escape(logesc, sizeof(logesc), logbuf);
        snprintf(body, sizeof(body), "{\"ok\":true,\"log\":\"%s\"}\n", logesc);
    } else if (is_post && !strcmp(path, "/on")) {
        g_state.power = true;
        state_changed();
        config_save();
        snprintf(body, sizeof(body), "{\"ok\":true,\"power\":true}\n");
    } else if (is_post && !strcmp(path, "/off")) {
        g_state.power = false;
        state_changed();
        config_save();
        snprintf(body, sizeof(body), "{\"ok\":true,\"power\":false}\n");
    } else if (is_post && !strcmp(path, "/toggle")) {
        g_state.power = !g_state.power;
        state_changed();
        config_save();
        snprintf(body, sizeof(body), "{\"ok\":true,\"power\":%s}\n",
                 g_state.power ? "true" : "false");
    } else if (is_post && !strcmp(path, "/effect")) {
        handle_effect(query, body, sizeof(body), &code);
    } else if (is_post && !strcmp(path, "/color")) {
        handle_color(query, body, sizeof(body), &code);
    } else if (is_post && !strcmp(path, "/preset")) {
        handle_preset(query, body, sizeof(body), &code);
    } else if (is_post && !strcmp(path, "/config")) {
        handle_config(query, body, sizeof(body), &code);
    } else if (is_post && !strcmp(path, "/brightness")) {
        int value = 0;
        bool found = false;
        if (!query_int(query, "value", 0, 100, &value, &found) || !found) {
            code = 400;
            snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"value must be 0..100\"}\n");
        } else {
            g_state.brightness = value;
            config_save();
            snprintf(body, sizeof(body), "{\"ok\":true,\"brightness\":%d}\n", value);
        }
    } else if (is_post && !strcmp(path, "/transition")) {
        int ms = g_state.transition_ms;
        bool found = false;
        if (!query_int(query, "ms", 0, 10000, &ms, &found)) {
            code = 400;
            snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"ms must be 0..10000\"}\n");
        } else {
            char style[32];
            int st = g_state.transition_style;
            if (query_get(query, "style", style, sizeof(style))) {
                st = transition_by_name(style);
                if (st < 0) {
                    code = 400;
                    snprintf(body, sizeof(body),
                             "{\"ok\":false,\"error\":\"unknown transition style\"}\n");
                }
            }
            if (code == 200) {
                g_state.transition_ms = ms;
                g_state.transition_style = st;
                config_save();
                snprintf(body, sizeof(body), "{\"ok\":true,\"ms\":%d,\"style\":\"%s\"}\n",
                         g_state.transition_ms, TR_NAMES[g_state.transition_style]);
            }
        }
    } else if (is_post && !strcmp(path, "/reactive")) {
        int enabled = g_state.reactive ? 1 : 0;
        int gain = g_state.audio_gain;
        bool found = false;
        if (!query_int(query, "enabled", 0, 1, &enabled, &found)) {
            code = 400;
            snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"enabled must be 0 or 1\"}\n");
        } else if (!query_int(query, "gain", 0, 400, &gain, &found)) {
            code = 400;
            snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"gain must be 0..400\"}\n");
        } else {
            g_state.reactive = enabled != 0;
            g_state.audio_gain = gain;
            config_save();
            snprintf(body, sizeof(body), "{\"ok\":true,\"reactive\":%s,\"gain\":%d}\n",
                     g_state.reactive ? "true" : "false", g_state.audio_gain);
        }
    } else if (is_get && !strcmp(path, "/capture.txt")) {
        /* Read the last capture back: the page shows a summary and a stick round
         * copies the text, so a UI-triggered probe is not lost in /rw_data. */
        char cap_path[320];
        snprintf(cap_path, sizeof(cap_path), "%s/hw_capture.txt", g_state_dir);
        static char capbuf[16385];
        static char capesc[24577];
        if (read_file(cap_path, capbuf, sizeof(capbuf)) < 0) capbuf[0] = 0;
        json_escape(capesc, sizeof(capesc), capbuf);
        snprintf(body, sizeof(body), "{\"ok\":true,\"file\":\"hw_capture.txt\",\"text\":\"%s\"}\n", capesc);
    } else if (is_post && !strcmp(path, "/capture")) {
        /* Two-phase factory-ambient discovery, on demand from the page: an
           * 8 s idle baseline, then a 15 s window during which the driver
           * toggles a light.  The page beeps at the window start, so the timing
         * is audible instead of a guess.  Detached: the reply returns at once
         * and the result lands in /rw_data/ambient/hw_capture.txt. */
        char script[400], cap_out[400], cmd[1200];
        snprintf(script, sizeof(script), "%s/hw_capture.sh", g_state_dir);
        snprintf(cap_out, sizeof(cap_out), "%s/hw_capture.txt", g_state_dir);
        write_atomic(script, HW_CAPTURE_SH);
        if (access(script, R_OK) != 0) {
            code = 500;
            snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"cannot write the capture script\"}\n");
        } else {
            chmod(script, 0700);
            unlink(cap_out);
            g_capture_started_ms = (int64_t)time(NULL) * 1000;
            snprintf(cmd, sizeof(cmd), "sh %s %s %s 8 15 >/dev/null 2>&1 &",
                     script, g_dbus_monitor, cap_out);
            if (system(cmd) != 0) {
                code = 500;
                snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"capture launch failed\"}\n");
            } else {
                snprintf(body, sizeof(body),
                         "{\"ok\":true,\"capture\":\"running\",\"baseline_s\":8,"
                         "\"toggle_in_s\":8,\"window_s\":15}\n");
            }
        }
    } else {
        code = 404;
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"not found\"}\n");
    }

    http_reply(fd, code, body);
    close(fd);
}

/* ------------------------------------------------------------------ main */

static void on_signal(int signum)
{
    (void)signum;
    g_stop = true;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--port 8094] [--state-dir DIR] [--no-restore] [--quiet]\n"
            "       %s [--backend none|file|wled|micom] [--target HOST]\n"
            "          [--target-port N] [--fps N] [--output-hz N]\n"
            "          [--micom-send PATH] [--micom-template 'AA BB %%R %%G %%B ...']\n"
            "       %s [--audio-cmd CMD | --audio-file PATH] [--audio-rate HZ]\n"
            "          [--audio-off] [--session-bus-file PATH] [--dbus-monitor PATH]\n"
            "          [--no-session-monitor] [--no-persist] [--status]\n"
            "          [--version] [--help]\n",
            argv0, argv0, argv0);
}

static void mkdir_state(void)
{
    mkdir(g_state_dir, 0755);
}

int main(int argc, char **argv)
{
    int port = HTTP_PORT;
    bool no_restore = false;
    bool one_shot = false;

    state_defaults();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--state-dir") && i + 1 < argc)
            snprintf(g_state_dir, sizeof(g_state_dir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-restore")) no_restore = true;
        else if (!strcmp(argv[i], "--no-persist")) g_persist = false;
        else if (!strcmp(argv[i], "--quiet")) g_log_quiet = true;
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            int bk = backend_by_name(argv[++i]);
            if (bk < 0) {
                fprintf(stderr, "unknown backend: %s\n", argv[i]);
                return 2;
            }
            g_backend = bk;
        }
        else if (!strcmp(argv[i], "--target") && i + 1 < argc)
            snprintf(g_target, sizeof(g_target), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--target-port") && i + 1 < argc)
            g_target_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--output-hz") && i + 1 < argc)
            g_output_hz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--micom-send") && i + 1 < argc)
            snprintf(g_micom_send, sizeof(g_micom_send), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--micom-template") && i + 1 < argc)
            snprintf(g_micom_template, sizeof(g_micom_template), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--audio-cmd") && i + 1 < argc)
            snprintf(g_audio_cmd, sizeof(g_audio_cmd), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--audio-file") && i + 1 < argc)
            snprintf(g_audio_file, sizeof(g_audio_file), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--audio-rate") && i + 1 < argc)
            g_audio_rate = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--audio-off")) {
            g_audio_cmd[0] = 0;
            g_audio_file[0] = 0;
            g_state.reactive = false;
        }
        else if (!strcmp(argv[i], "--session-bus-file") && i + 1 < argc)
            snprintf(g_session_bus_file, sizeof(g_session_bus_file), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--dbus-monitor") && i + 1 < argc)
            snprintf(g_dbus_monitor, sizeof(g_dbus_monitor), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-session-monitor")) g_mon_enabled = false;
        else if (!strcmp(argv[i], "--status")) one_shot = true;
        else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("ambient_daemon %s\n", AMBIENT_DAEMON_VERSION);
            return 0;
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        }
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    mkdir_state();
    snprintf(g_config_path, sizeof(g_config_path), "%s/config.conf", g_state_dir);
    snprintf(g_status_path, sizeof(g_status_path), "%s/status.json", g_state_dir);
    snprintf(g_presets_path, sizeof(g_presets_path), "%s/presets.conf", g_state_dir);
    snprintf(g_log_path, sizeof(g_log_path), "%s/daemon.log", g_state_dir);

    if (!no_restore) config_load();
    state_clamp();
    presets_load();

    if (g_fps < 1 || g_fps > 120) g_fps = 25;
    if (g_output_hz < 1 || g_output_hz > 120) g_output_hz = 20;
    if (g_target_port < 1 || g_target_port > 65535) g_target_port = 80;

    g_audio.fd = -1;
    g_audio.pid = -1;
    g_audio.rate = g_audio_rate;
    biquad_init(&g_audio.bass, g_audio_rate, 60.0f, 1.0f);
    biquad_init(&g_audio.mid, g_audio_rate, 450.0f, 1.0f);
    biquad_init(&g_audio.high, g_audio_rate, 3000.0f, 1.0f);

    g_phase_ms = mono_ms();
    g_trans_start = g_phase_ms;
    memset(g_last_frame, 0, sizeof(g_last_frame));
    memset(g_from_frame, 0, sizeof(g_from_frame));

    if (one_shot) {
        rgb_t frame[ZONES_MAX];
        render_frame(mono_ms(), frame);
        static char out[8192];
        build_status(out, sizeof(out));
        fputs(out, stdout);
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    fcntl(srv, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind(http)");
        close(srv);
        return 1;
    }
    if (listen(srv, 8) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    log_msg("v%s, http 127.0.0.1:%d, backend=%s target=%s state=%s",
            AMBIENT_DAEMON_VERSION, port, backend_name(),
            g_target[0] ? g_target : "-", g_state_dir);
    log_cap();

    int64_t last_render = 0;
    int64_t last_status = 0;
    int64_t status_interval = 1000;

    while (!g_stop) {
        struct pollfd pfd = { .fd = srv, .events = POLLIN };
        int pr = poll(&pfd, 1, 20);
        int64_t now = mono_ms();

        if (pr > 0 && (pfd.revents & POLLIN)) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                fcntl(c, F_SETFD, FD_CLOEXEC);
                struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                http_handle(c);
            }
        }

        reap_children();
        monitor_tick();
        audio_tick();

        int64_t frame_interval = 1000 / (g_fps > 0 ? g_fps : 1);
        if (now - last_render >= frame_interval) {
            last_render = now;
            rgb_t frame[ZONES_MAX];
            render_frame(now, frame);
            tick_output(now, frame);
        }

        if (now - last_status >= status_interval) {
            last_status = now;
            write_status_file();
            log_cap();
        }
    }

    if (g_mon_fd >= 0) close(g_mon_fd);
    if (g_audio.fd >= 0) close(g_audio.fd);
    close(srv);
    log_msg("stopped");
    return 0;
}
