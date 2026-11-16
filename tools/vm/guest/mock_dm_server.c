/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * mock_dm_server - loopback-only lab DM endpoint for dmengine tests.
 *
 * Listens on 127.0.0.1, accepts one request at a time, and logs a structured
 * summary of the request (method, path, content type, length and a short body
 * prefix). For WBXML DM requests it extracts the SessionID, MsgID, target URL
 * and IMEI, then returns a minimal WBXML SyncML DM response with Status entries
 * for the captured Alert/Replace commands and a Final element.
 *
 * In download mode it also serves an OMA Download Descriptor at
 * /package.bin and a minimal RedBend EF DP object (preamble, version 4, one
 * component) at /object.bin. The DP is a structural lab fixture: it has no
 * component table or signature and is not installable.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_REQUEST (1024 * 1024)
#define BODY_PREFIX 24
#define MAX_STRINGS 32
#define MAX_STRING 256
#define MAX_RESPONSE (16 * 1024)

#define TOKEN_SYNCML 0x6d
#define TOKEN_SYNCHDR 0x6c
#define TOKEN_VERDTD 0x71
#define TOKEN_VERPROTO 0x72
#define TOKEN_SESSIONID 0x65
#define TOKEN_MSGID 0x5b
#define TOKEN_TARGET 0x6e
#define TOKEN_LOCURI 0x57
#define TOKEN_SOURCE 0x67
#define TOKEN_SYNCBODY 0x6b
#define TOKEN_STATUS 0x69
#define TOKEN_CMDID 0x4b
#define TOKEN_MSGREF 0x5c
#define TOKEN_CMDREF 0x4c
#define TOKEN_CMD 0x4a
#define TOKEN_DATA 0x4f
#define TOKEN_FINAL 0x12
#define TOKEN_EXEC 0x51
#define TOKEN_CRED 0x4e
#define TOKEN_META 0x5a
#define TOKEN_FORMAT 0x47
#define TOKEN_TYPE 0x53
#define WBXML_END 0x01
#define WBXML_STRING 0x03
#define WBXML_OPAQUE 0xc3

struct wbxml_string {
    size_t offset;
    size_t length;
    char value[MAX_STRING];
};

struct buffer {
    unsigned char data[MAX_RESPONSE];
    size_t length;
};

static volatile sig_atomic_t running = 1;

static void stop(int sig)
{
    (void)sig;
    running = 0;
}

static int read_request(int fd, unsigned char *buffer, size_t capacity,
                        size_t *body_offset, size_t *content_length)
{
    size_t used = 0;
    *body_offset = 0;
    *content_length = 0;
    while (used + 1 < capacity) {
        ssize_t got = read(fd, buffer + used, capacity - used - 1);
        if (got <= 0)
            break;
        used += (size_t)got;
        buffer[used] = '\0';
        unsigned char *header_end = (unsigned char *)strstr((char *)buffer,
                                                            "\r\n\r\n");
        if (header_end != NULL) {
            *body_offset = (size_t)(header_end - buffer) + 4;
            char *line = strcasestr((char *)buffer, "Content-Length:");
            if (line != NULL && (unsigned char *)line < header_end)
                *content_length = (size_t)strtoul(line + 15, NULL, 10);
            if (used >= *body_offset + *content_length)
                break;
        }
    }
    return (int)used;
}

static void header_value(const char *buffer, const char *name,
                         char *out, size_t out_size)
{
    const char *line = strcasestr(buffer, name);
    size_t used = 0;
    out[0] = '\0';
    if (line == NULL)
        return;
    line += strlen(name);
    while (*line == ' ' || *line == '\t')
        line++;
    while (*line && *line != '\r' && *line != '\n' && used + 1 < out_size)
        out[used++] = *line++;
    out[used] = '\0';
}

static int collect_wbxml_strings(const unsigned char *body, size_t length,
                                 struct wbxml_string *strings, int max)
{
    int count = 0;
    size_t index = 0;
    while (index + 2 < length && count < max) {
        if (body[index] == 0xC3) {
            size_t string_length = body[index + 1];
            if (string_length > 0 && string_length < MAX_STRING
                && index + 2 + string_length <= length) {
                strings[count].offset = index;
                strings[count].length = string_length;
                memcpy(strings[count].value, body + index + 2, string_length);
                strings[count].value[string_length] = '\0';
                count++;
                index += 2 + string_length;
                continue;
            }
        }
        index++;
    }
    return count;
}

static const char *find_string(const struct wbxml_string *strings, int count,
                               const char *prefix)
{
    for (int index = 0; index < count; index++) {
        if (strncmp(strings[index].value, prefix, strlen(prefix)) == 0)
            return strings[index].value;
    }
    return NULL;
}

static const char *find_digest(const struct wbxml_string *strings, int count)
{
    for (int index = 0; index < count; index++) {
        if (strings[index].length == 24
            && strings[index].value[22] == '='
            && strings[index].value[23] == '=')
            return strings[index].value;
    }
    return NULL;
}

static int find_msgid(const unsigned char *body, size_t length, size_t after,
                      char *out, size_t out_size)
{
    for (size_t index = after; index + 2 < length; index++) {
        if (body[index] != 0x5B)
            continue;
        size_t cursor = index + 1;
        if (cursor < length && body[cursor] == 0x01)
            cursor++;
        if (cursor + 1 < length && body[cursor] == 0x03) {
            size_t string_length = body[cursor + 1];
            if (string_length > 0 && string_length + 1 < out_size
                && cursor + 2 + string_length <= length) {
                memcpy(out, body + cursor + 2, string_length);
                out[string_length] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

static void put_byte(struct buffer *buffer, unsigned char value)
{
    if (buffer->length < sizeof(buffer->data))
        buffer->data[buffer->length++] = value;
}

static void put_string(struct buffer *buffer, const char *value)
{
    put_byte(buffer, WBXML_STRING);
    for (const char *cursor = value; *cursor; cursor++)
        put_byte(buffer, (unsigned char)*cursor);
    put_byte(buffer, 0x00);
}

static void put_opaque(struct buffer *buffer, const char *value)
{
    size_t length = strlen(value);
    if (length > 0x7f)
        return;
    put_byte(buffer, WBXML_OPAQUE);
    put_byte(buffer, (unsigned char)length);
    for (size_t index = 0; index < length; index++)
        put_byte(buffer, (unsigned char)value[index]);
}

static void put_status(struct buffer *buffer, const char *cmdid,
                       const char *cmdref, const char *command)
{
    put_byte(buffer, TOKEN_STATUS);
    put_byte(buffer, TOKEN_CMDID);
    put_string(buffer, cmdid);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, TOKEN_MSGREF);
    put_string(buffer, "1");
    put_byte(buffer, WBXML_END);
    put_byte(buffer, TOKEN_CMDREF);
    put_string(buffer, cmdref);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, TOKEN_CMD);
    put_string(buffer, command);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, TOKEN_DATA);
    put_string(buffer, "200");
    put_byte(buffer, WBXML_END);
    put_byte(buffer, WBXML_END);
}

static void put_exec(struct buffer *buffer, const char *locuri,
                        const char *pkgurl)
{
    (void)pkgurl;
    put_byte(buffer, TOKEN_EXEC);
    put_byte(buffer, TOKEN_CMDID);
    put_string(buffer, "4");
    put_byte(buffer, WBXML_END);
    put_byte(buffer, 0x54);             /* Item */
    put_byte(buffer, TOKEN_TARGET);
    put_byte(buffer, TOKEN_LOCURI);
    put_opaque(buffer, locuri);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, WBXML_END);        /* Item */
    put_byte(buffer, WBXML_END);        /* Exec */
}

static void put_server_cred(struct buffer *buffer, const char *digest)
{
    /* Mirror the client's Cred layout exactly. */
    static const unsigned char prefix[] = {
        TOKEN_CRED, TOKEN_META, 0x00, WBXML_END,
        TOKEN_FORMAT, WBXML_OPAQUE, 0x03, 'b', '6', '4', WBXML_END,
        TOKEN_TYPE, WBXML_OPAQUE, 0x0f, 's', 'y', 'n', 'c', 'm', 'l', ':',
        'a', 'u', 't', 'h', '-', 'm', 'd', '5', WBXML_END, WBXML_END,
        0x00, 0x00, TOKEN_DATA
    };
    for (size_t index = 0; index < sizeof(prefix); index++)
        put_byte(buffer, prefix[index]);
    put_opaque(buffer, digest);
    put_byte(buffer, WBXML_END);
    put_byte(buffer, WBXML_END);
}

static int build_response(const unsigned char *body, size_t length,
                          unsigned char *output, size_t capacity,
                          char *session, size_t session_size,
                          int download_mode, int send_exec, int send_update,
                          int combined_mode)
{
    static const char string_table[] = "-//SYNCML//DTD SyncML 1.2//EN";
    struct wbxml_string strings[MAX_STRINGS];
    struct buffer buffer;
    int count = collect_wbxml_strings(body, length, strings, MAX_STRINGS);
    const char *url = find_string(strings, count, "http");
    const char *imei = find_string(strings, count, "IMEI:");
    char msgid[32] = "1";
    size_t session_offset;
    if (count == 0)
        return 0;
    const char *sid = strings[0].value;
    session_offset = strings[0].offset;
    if (sid[0] == '\0')
        return 0;
    if (url == NULL)
        url = "http://127.0.0.1:8002/dm/message";
    if (imei == NULL)
        imei = "IMEI:LAB000000000000";
    find_msgid(body, length, session_offset, msgid, sizeof(msgid));
    if (strlen(sid) >= session_size)
        return 0;
    memcpy(session, sid, strlen(sid) + 1);
    if (capacity < 5 + sizeof(string_table) + 512)
        return 0;
    buffer.length = 0;
    put_byte(&buffer, 0x02);            /* WBXML 1.2 */
    put_byte(&buffer, 0xa4);            /* SyncML 1.2 public ID */
    put_byte(&buffer, 0x01);
    put_byte(&buffer, 0x6a);            /* UTF-8 */
    put_byte(&buffer, (unsigned char)(sizeof(string_table) - 1));
    for (size_t index = 0; index + 1 < sizeof(string_table); index++)
        put_byte(&buffer, (unsigned char)string_table[index]);

    put_byte(&buffer, TOKEN_SYNCML);
    put_byte(&buffer, TOKEN_SYNCHDR);
    put_byte(&buffer, TOKEN_VERDTD);
    put_string(&buffer, "1.2");
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_VERPROTO);
    put_string(&buffer, "DM/1.2");
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_SESSIONID);
    put_opaque(&buffer, sid);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_MSGID);
    put_string(&buffer, "2");
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_TARGET);
    put_byte(&buffer, TOKEN_LOCURI);
    put_opaque(&buffer, imei);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, TOKEN_SOURCE);
    put_byte(&buffer, TOKEN_LOCURI);
    put_opaque(&buffer, url);
    put_byte(&buffer, WBXML_END);
    put_byte(&buffer, WBXML_END);
    if (download_mode) {
        const char *digest = find_digest(strings, count);
        if (digest != NULL) {
            FILE *auth = fopen("/tmp/mock-dm-auth.log", "a");
            if (auth != NULL) {
                fprintf(auth, "CLIENT_DIGEST=%s\n", digest);
                fclose(auth);
            }
            put_server_cred(&buffer, digest);
        }
    }
    put_byte(&buffer, WBXML_END);       /* SyncHdr */
    put_byte(&buffer, TOKEN_SYNCBODY);
    put_status(&buffer, "1", "1", "Alert");
    put_status(&buffer, "2", "2", "Replace");
    put_status(&buffer, "3", "3", "Alert");
    if (send_update)
        put_exec(&buffer, "./ManagedObjects/FUMO/Update", "");
    else if (send_exec)
        put_exec(&buffer, combined_mode
                    ? "./ManagedObjects/FUMO/DownloadAndUpdate"
                    : "./ManagedObjects/FUMO/Download",
                    "http://127.0.0.1:8002/package.bin");
    put_byte(&buffer, TOKEN_FINAL);
    put_byte(&buffer, WBXML_END);       /* SyncBody */
    put_byte(&buffer, WBXML_END);       /* SyncML */
    if (buffer.length > capacity)
        return 0;
    memcpy(output, buffer.data, buffer.length);
    return (int)buffer.length;
}

static void send_all(int fd, const unsigned char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t written = write(fd, data + sent, length - sent);
        if (written <= 0)
            break;
        sent += (size_t)written;
    }
}

static void put_u32(unsigned char *out, size_t offset, unsigned int value)
{
    out[offset] = (unsigned char)(value & 0xff);
    out[offset + 1] = (unsigned char)((value >> 8) & 0xff);
    out[offset + 2] = (unsigned char)((value >> 16) & 0xff);
    out[offset + 3] = (unsigned char)((value >> 24) & 0xff);
}

/* Structural RedBend EF DP: 20-byte preamble, version 4, one 28-byte
 * component entry, one update-table entry, a NUL-terminated string table and
 * a 16-byte update payload. A 52-byte lab HMAC-SHA256 trailer follows the
 * 132-byte DP body; this is a lab fixture, not a vendor signature. */
static int build_dp_package(unsigned char *out, size_t capacity)
{
    static const char preamble[] = "RedBendEFDPackage";
    const size_t header_length = 52;
    const size_t entry_length = 28;
    const size_t update_base = header_length + entry_length;
    const size_t string_base = update_base + 8;
    const size_t payload_base = string_base + 28;
    const size_t body_length = payload_base + 16;
    const size_t total_length = body_length + 52;
    static const unsigned char signature[32] = {
        0x06, 0xb4, 0xa8, 0xa0, 0x42, 0x86, 0xdd, 0xaf,
        0x29, 0x7b, 0x13, 0x91, 0xf8, 0xe3, 0x02, 0x3b,
        0x3f, 0x73, 0xf7, 0xcf, 0x5a, 0xe3, 0x7c, 0x73,
        0x9a, 0x00, 0x58, 0xa0, 0x88, 0x06, 0x1f, 0x6c
    };
    if (capacity < total_length)
        return 0;
    memset(out, 0, total_length);
    memcpy(out, preamble, sizeof(preamble) - 1);
    put_u32(out, 20, 4);            /* DP version */
    put_u32(out, 24, 1);            /* component count */
    put_u32(out, 28, (unsigned int)body_length);
    put_u32(out, 32, 0);            /* delta table pointer */
    put_u32(out, 36, 0);            /* scripts pointer */
    put_u32(out, 44, (unsigned int)header_length); /* component table */
    put_u32(out, 48, (unsigned int)string_base);   /* string table */
    put_u32(out, 52, (unsigned int)string_base);       /* name: LABCOMP */
    put_u32(out, 56, (unsigned int)string_base + 8);   /* id: LABID */
    put_u32(out, 60, (unsigned int)string_base + 14);  /* version: 1.0 */
    put_u32(out, 64, (unsigned int)string_base + 18);  /* vendor */
    put_u32(out, 68, 0);            /* installer type */
    put_u32(out, 72, 1);            /* update count */
    put_u32(out, 76, (unsigned int)update_base);       /* update table */
    put_u32(out, 80, (unsigned int)payload_base);      /* payload offset */
    put_u32(out, 84, 16);           /* payload size */
    memcpy(out + string_base, "LABCOMP", 8);
    memcpy(out + string_base + 8, "LABID", 6);
    memcpy(out + string_base + 14, "1.0", 4);
    memcpy(out + string_base + 18, "LABVENDOR", 10);
    memcpy(out + payload_base, "LABUPDATE-DATA01", 16);
    memcpy(out + body_length, "RedBendEFDSig", 13);
    put_u32(out, (unsigned int)body_length + 16, 32);
    memcpy(out + body_length + 20, signature, sizeof(signature));
    return (int)total_length;
}

/* Multi-component lab DP: two 28-byte component entries, three update-table
 * entries, eight strings, three 16-byte payloads and a 52-byte lab HMAC
 * trailer. Layout: header 52, components 56, updates 24, strings 59,
 * payloads 48, trailer 52 (total 291). This is a lab fixture, not bootable. */
static int build_dp_multi_package(unsigned char *out, size_t capacity)
{
    static const char preamble[] = "RedBendEFDPackage";
    const size_t body_length = 239;
    const size_t total_length = 291;
    static const unsigned char signature[32] = {
        0x35, 0x42, 0x95, 0x96, 0xf0, 0x3e, 0xff, 0xd7,
        0x7c, 0x17, 0x41, 0xd8, 0x6a, 0x47, 0x56, 0x6f,
        0x39, 0x00, 0xae, 0x28, 0xbd, 0x3e, 0x1e, 0x3b,
        0x00, 0xcb, 0x56, 0xc4, 0x5f, 0x4b, 0x80, 0x3a
    };
    if (capacity < total_length)
        return 0;
    memset(out, 0, total_length);
    memcpy(out, preamble, sizeof(preamble) - 1);
    put_u32(out, 20, 4);
    put_u32(out, 24, 2);
    put_u32(out, 28, (unsigned int)body_length);
    put_u32(out, 32, 0);
    put_u32(out, 36, 0);
    put_u32(out, 44, 52);
    put_u32(out, 48, 132);
    put_u32(out, 52, 132);
    put_u32(out, 56, 140);
    put_u32(out, 60, 146);
    put_u32(out, 64, 150);
    put_u32(out, 68, 0);
    put_u32(out, 72, 2);
    put_u32(out, 76, 108);
    put_u32(out, 80, 160);
    put_u32(out, 84, 169);
    put_u32(out, 88, 176);
    put_u32(out, 92, 180);
    put_u32(out, 96, 1);
    put_u32(out, 100, 1);
    put_u32(out, 104, 124);
    put_u32(out, 108, 191);
    put_u32(out, 112, 16);
    put_u32(out, 116, 207);
    put_u32(out, 120, 16);
    put_u32(out, 124, 223);
    put_u32(out, 128, 16);
    memcpy(out + 132, "LABCOMP", 8);
    memcpy(out + 140, "LABID", 6);
    memcpy(out + 146, "1.0", 4);
    memcpy(out + 150, "LABVENDOR", 10);
    memcpy(out + 160, "LABCOMP2", 9);
    memcpy(out + 169, "LABID2", 7);
    memcpy(out + 176, "2.0", 4);
    memcpy(out + 180, "LABVENDOR2", 11);
    memcpy(out + 191, "LABUPDATE-DATA01", 16);
    memcpy(out + 207, "LABUPDATE-DATA02", 16);
    memcpy(out + 223, "LABUPDATE-DATA03", 16);
    memcpy(out + body_length, "RedBendEFDSig", 13);
    put_u32(out, (unsigned int)body_length + 16, 32);
    memcpy(out + body_length + 20, signature, sizeof(signature));
    return (int)total_length;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8002;
    const char *log_path = argc > 2 ? argv[2] : "/tmp/mock-dm.log";
    int update_mode = argc > 3 && strcmp(argv[3], "update") == 0;
    int combined_mode = argc > 3
        && (strcmp(argv[3], "combined") == 0
            || strcmp(argv[3], "combinedmulti") == 0);
    int missing_mode = argc > 3 && strcmp(argv[3], "missing") == 0;
    int notify_fail_mode = argc > 3 && strcmp(argv[3], "notifyfail") == 0;
    int badsig_mode = argc > 3 && strcmp(argv[3], "badsig") == 0;
    int partial_mode = argc > 3 && strcmp(argv[3], "partial") == 0;
    int multiresume_mode = argc > 3 && strcmp(argv[3], "multiresume") == 0;
    int resume_mode = argc > 3
        && (strcmp(argv[3], "resume") == 0 || multiresume_mode);
    int noetag_mode = argc > 3 && strcmp(argv[3], "noetag") == 0;
    int multidp_mode = argc > 3
        && (strcmp(argv[3], "multidp") == 0
            || strcmp(argv[3], "combinedmulti") == 0
            || multiresume_mode);
    int download_mode = argc > 3
        && (strcmp(argv[3], "download") == 0 || update_mode
            || combined_mode || missing_mode || notify_fail_mode
            || badsig_mode || partial_mode || resume_mode || noetag_mode
            || multidp_mode);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in addr;
    FILE *log;
    int requests = 0;
    if (server < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server, 8) < 0) {
        perror("listen");
        return 1;
    }
    log = fopen(log_path, "a");
    if (log == NULL) {
        perror("log");
        return 1;
    }
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    printf("MOCK_DM_READY port=%d\n", port);
    fflush(stdout);
    while (running) {
        int client = accept(server, NULL, NULL);
        unsigned char *request = malloc(MAX_REQUEST);
        size_t body_offset = 0;
        size_t content_length = 0;
        if (client < 0)
            continue;
        if (request == NULL) {
            close(client);
            continue;
        }
        int used = read_request(client, request, MAX_REQUEST,
                                &body_offset, &content_length);
        char method[16];
        char path[256];
        char content_type[128];
        char prefix[BODY_PREFIX * 3 + 1];
        char session[64] = "";
        unsigned char response[MAX_RESPONSE];
        int response_body = 0;
        size_t prefix_len = 0;
        header_value((char *)request, "Content-Type:", content_type,
                     sizeof(content_type));
        method[0] = '\0';
        path[0] = '\0';
        if (used > 0) {
            char *space = strchr((char *)request, ' ');
            if (space != NULL) {
                size_t method_len = (size_t)(space - (char *)request);
                if (method_len < sizeof(method)) {
                    memcpy(method, request, method_len);
                    method[method_len] = '\0';
                }
                char *path_start = space + 1;
                char *path_end = strchr(path_start, ' ');
                if (path_end != NULL) {
                    size_t path_len = (size_t)(path_end - path_start);
                    if (path_len < sizeof(path)) {
                        memcpy(path, path_start, path_len);
                        path[path_len] = '\0';
                    }
                }
            }
        }
        for (size_t index = 0; index < BODY_PREFIX
             && body_offset + index < (size_t)used; index++) {
            prefix_len += (size_t)snprintf(prefix + prefix_len,
                                           sizeof(prefix) - prefix_len,
                                           "%02x", request[body_offset + index]);
        }
        prefix[prefix_len] = '\0';
        if (strcmp(method, "GET") == 0 && strcmp(path, "/package.bin") == 0) {
            unsigned char package[512];
            char dd[768];
            char header[256];
            int header_length;
            int package_length = multidp_mode
                ? build_dp_multi_package(package, sizeof(package))
                : build_dp_package(package, sizeof(package));
            int dd_length = snprintf(
                dd, sizeof(dd),
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<media xmlns=\"http://www.openmobilealliance.org/xmlns/dd\" "
                "UIInfo=\"lab-ui\">"
                "<ddVersion>1.0</ddVersion>"
                "<type>application/octet-stream</type>"
                "<size>%d</size>"
                "<objectURI>http://127.0.0.1:8002/object.bin</objectURI>"
                "<installNotifyURI>http://127.0.0.1:8002/notify</installNotifyURI>"
                "<name>lab-fumo</name><vendor>lab</vendor>"
                "<description>lab FUMO package</description>"
                "</media>", package_length);
            header_length = snprintf(
                header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/vnd.oma.dd+xml\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", dd_length);
            send_all(client, (const unsigned char *)header,
                     (size_t)header_length);
            send_all(client, (const unsigned char *)dd, (size_t)dd_length);
            fprintf(log, "MOCK_DM_DD method=GET path=/package.bin bytes=%d "
                         "object=/object.bin object_bytes=%d\n",
                    dd_length, package_length);
            fflush(log);
            close(client);
            free(request);
            requests++;
            fprintf(log, "MOCK_DM_DD_DONE requests=%d\n", requests);
            fflush(log);
            continue;
        }
        if (strcmp(method, "POST") == 0 && strcmp(path, "/notify") == 0) {
            static const char ok[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            char body[64];
            size_t notify_length = (size_t)used > body_offset
                ? (size_t)used - body_offset : 0;
            size_t copy_length = notify_length < sizeof(body) - 1
                ? notify_length : sizeof(body) - 1;
            memcpy(body, request + body_offset, copy_length);
            body[copy_length] = '\0';
            if (notify_fail_mode) {
                static const char fail[] =
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                send_all(client, (const unsigned char *)fail, sizeof(fail) - 1);
                fprintf(log, "MOCK_DM_NOTIFY_FAIL path=/notify bytes=%zu body=%s\n",
                        notify_length, body);
            } else {
                send_all(client, (const unsigned char *)ok, sizeof(ok) - 1);
                fprintf(log, "MOCK_DM_NOTIFY path=/notify bytes=%zu body=%s\n",
                        notify_length, body);
            }
            fflush(log);
            close(client);
            free(request);
            requests++;
            continue;
        }
        if (strcmp(method, "GET") == 0 && strcmp(path, "/object.bin") == 0) {
            unsigned char package[512];
            if (missing_mode) {
                static const char not_found[] =
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                send_all(client, (const unsigned char *)not_found,
                         sizeof(not_found) - 1);
                fprintf(log, "MOCK_DM_MISSING method=GET path=/object.bin\n");
                fflush(log);
                close(client);
                free(request);
                requests++;
                continue;
            }
            char header[256];
            int header_length;
            int package_length = multidp_mode
                ? build_dp_multi_package(package, sizeof(package))
                : build_dp_package(package, sizeof(package));
            if (badsig_mode && package_length > 0)
                package[package_length - 1] ^= 0x01;
            if (noetag_mode) {
                static int noetag_object_requests = 0;
                char *range_header = strcasestr((char *)request, "Range:");
                if (noetag_object_requests++ == 0) {
                    header_length = snprintf(
                        header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n", package_length);
                    send_all(client, (const unsigned char *)header,
                             (size_t)header_length);
                    send_all(client, package, 64);
                    fprintf(log, "MOCK_DM_NOETAG_PARTIAL method=GET "
                                 "path=/object.bin bytes=64 total=%d\n",
                            package_length);
                } else {
                    header_length = snprintf(
                        header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n", package_length);
                    send_all(client, (const unsigned char *)header,
                             (size_t)header_length);
                    send_all(client, package, (size_t)package_length);
                    fprintf(log, "MOCK_DM_NOETAG_FULL method=GET "
                                 "path=/object.bin bytes=%d range=%s\n",
                            package_length,
                            range_header != NULL ? "present" : "none");
                }
                fflush(log);
                close(client);
                free(request);
                requests++;
                continue;
            }
            if (partial_mode || resume_mode) {
                char *range_header = strcasestr((char *)request, "Range:");
                const char *etag = multidp_mode ? "lab-dp-291" : "lab-dp-184";
                unsigned long range_offset = 0;
                fprintf(log, "MOCK_DM_OBJECT_HEADERS range=%s\n",
                        range_header != NULL ? range_header : "none");
                fflush(log);
                if (range_header != NULL) {
                    char *equals = strchr(range_header, '=');
                    if (equals != NULL)
                        range_offset = strtoul(equals + 1, NULL, 10);
                }
                if (resume_mode && range_header != NULL
                    && range_offset > 0
                    && range_offset < (unsigned long)package_length) {
                    header_length = snprintf(
                        header, sizeof(header),
                        "HTTP/1.1 206 Partial Content\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Range: bytes %lu-%d/%d\r\n"
                        "ETag: \"%s\"\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n",
                        range_offset, package_length - 1, package_length,
                        etag, package_length - (int)range_offset);
                    send_all(client, (const unsigned char *)header,
                             (size_t)header_length);
                    send_all(client, package + range_offset,
                             (size_t)(package_length - (int)range_offset));
                    fprintf(log, "MOCK_DM_RESUME method=GET path=/object.bin "
                                 "offset=%lu bytes=%d total=%d\n",
                            range_offset,
                            package_length - (int)range_offset, package_length);
                    fflush(log);
                    close(client);
                    free(request);
                    requests++;
                    continue;
                }
                header_length = snprintf(
                    header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Accept-Ranges: bytes\r\n"
                    "ETag: \"%s\"\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n", etag, package_length);
                send_all(client, (const unsigned char *)header,
                         (size_t)header_length);
                send_all(client, package, 64);
                fprintf(log, "MOCK_DM_PARTIAL method=GET path=/object.bin "
                             "bytes=64 total=%d\n", package_length);
                fflush(log);
                close(client);
                free(request);
                requests++;
                continue;
            }
            header_length = snprintf(
                header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", package_length);
            send_all(client, (const unsigned char *)header,
                     (size_t)header_length);
            send_all(client, package, (size_t)package_length);
            fprintf(log, "MOCK_DM_DOWNLOAD method=GET path=/object.bin "
                         "bytes=%d preamble=RedBendEFDPackage version=4 "
                         "components=%d\n", package_length,
                        multidp_mode ? 2 : 1);
            if (multidp_mode)
                fprintf(log, "MOCK_DM_MULTIDP method=GET path=/object.bin "
                             "bytes=%d components=2 updates=3\n",
                        package_length);
            if (badsig_mode)
                fprintf(log, "MOCK_DM_BADSIG path=/object.bin bytes=%d\n",
                        package_length);
            fflush(log);
            close(client);
            free(request);
            requests++;
            fprintf(log, "MOCK_DM_DOWNLOAD_DONE requests=%d\n", requests);
            fflush(log);
            continue;
        }
        if (requests == 1 && (size_t)used > body_offset) {
            FILE *second = fopen("/tmp/dm-request2.bin", "wb");
            if (second != NULL) {
                fwrite(request + body_offset, 1,
                       (size_t)used - body_offset, second);
                fclose(second);
            }
            for (size_t index = 0; index + 3 <= (size_t)used - body_offset;
                 index++) {
                if (request[body_offset + index] == '4'
                    && request[body_offset + index + 1] == '0'
                    && request[body_offset + index + 2] == '7') {
                    fprintf(log, "MOCK_DM_CLIENT_STATUS status=407\n");
                    fflush(log);
                    break;
                }
            }
        }
        if (strcmp(method, "POST") == 0 && content_length > 900
            && requests >= 2 && (size_t)used > body_offset) {
            FILE *third = fopen("/tmp/dm-request3.bin", "wb");
            if (third != NULL) {
                fwrite(request + body_offset, 1,
                       (size_t)used - body_offset, third);
                fclose(third);
            }
        }
        if ((size_t)used > body_offset) {
            int report = download_mode && content_length > 900
                         && requests >= 2;
            response_body = build_response(
                request + body_offset, (size_t)used - body_offset,
                response, sizeof(response), session, sizeof(session),
                download_mode, download_mode && requests == 0,
                update_mode && report, combined_mode);
            if (update_mode && report)
                fprintf(log, "MOCK_DM_UPDATE_EXEC path=/dm/message\n");
        }
        fprintf(log,
                "MOCK_DM_REQUEST method=%s path=%s content_type=%s "
                "content_length=%zu body_prefix=%s\n",
                method, path, content_type, content_length, prefix);
        if (response_body > 0) {
            char header[256];
            int header_length = snprintf(
                header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/vnd.syncml.dm+wbxml\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", response_body);
            send_all(client, (const unsigned char *)header,
                     (size_t)header_length);
            send_all(client, response, (size_t)response_body);
            fprintf(log,
                    "MOCK_DM_RESPONSE status=200 syncml=1 session=%s "
                    "bytes=%d exec=%d\n", session, response_body,
                    download_mode && requests == 0);
        } else {
            static const char empty_response[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/vnd.syncml.dm+wbxml\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send_all(client, (const unsigned char *)empty_response,
                     sizeof(empty_response) - 1);
            fprintf(log, "MOCK_DM_RESPONSE status=200 syncml=0\n");
        }
        fflush(log);
        close(client);
        free(request);
        requests++;
        fprintf(log, "MOCK_DM_RESPONSE_DONE requests=%d\n", requests);
        fflush(log);
    }
    fclose(log);
    close(server);
    return 0;
}
