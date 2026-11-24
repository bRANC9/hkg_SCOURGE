/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors */
/* SPDX-License-Identifier: AGPL-3.0-only */
/* Minimal HTTP/1.0 client for the unit: busybox has no wget and no nc, so the
 * installer could not drive the daemon API at all (api_walk reported
 * "no wget/nc" for every step).  Static, musl, baseline ISA like everything
 * else we ship.
 *
 * usage: httpreq GET|POST <host> <port> <path>   -> prints the body
 * exit: 0 on 2xx, 1 otherwise, 2 on usage/connection error. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s GET|POST host port path\n", argv[0]);
        return 2;
    }
    const char *method = argv[1];
    const char *host = argv[2];
    int port = atoi(argv[3]);
    const char *path = argv[4];

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 2;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return 2; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return 2; }

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.0\r\nHost: %s\r\nContent-Length: 0\r\n\r\n",
                     method, path, host);
    if (n <= 0 || write(fd, req, (size_t)n) != n) { close(fd); return 2; }

    char buf[4096];
    int status = 0, header_done = 0;
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        if (!header_done) {
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) continue;
            header_done = 1;
            sscanf(buf, "HTTP/%*d.%*d %d", &status);
            fwrite(end + 4, 1, (size_t)((buf + r) - (end + 4)), stdout);
        } else {
            fwrite(buf, 1, (size_t)r, stdout);
        }
    }
    close(fd);
    fflush(stdout);
    return (status >= 200 && status < 300) ? 0 : 1;
}
