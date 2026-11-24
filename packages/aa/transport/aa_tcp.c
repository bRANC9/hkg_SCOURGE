/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * TCP transport implementation.  See aa_tcp.h for the contract.
 */
#include "aa_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

aa_tcp_status aa_tcp_open(aa_tcp_listener *listener, const char *bind_address, uint16_t port)
{
    if (listener == NULL)
        return AA_TCP_ERR_ARGUMENT;
    listener->listener = -1;
    listener->port = 0u;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return AA_TCP_ERR_IO;

    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (bind_address == NULL || bind_address[0] == '\0') {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        close(fd);
        return AA_TCP_ERR_ARGUMENT;
    }

    if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        close(fd);
        return AA_TCP_ERR_IO;
    }
    if (listen(fd, 4) < 0) {
        close(fd);
        return AA_TCP_ERR_IO;
    }

    socklen_t address_len = sizeof address;
    if (getsockname(fd, (struct sockaddr *)&address, &address_len) < 0) {
        close(fd);
        return AA_TCP_ERR_IO;
    }
    listener->listener = fd;
    listener->port = ntohs(address.sin_port);
    return AA_TCP_OK;
}

void aa_tcp_close(aa_tcp_listener *listener)
{
    if (listener == NULL || listener->listener < 0)
        return;
    close(listener->listener);
    listener->listener = -1;
    listener->port = 0u;
}

static aa_tcp_status write_all(int fd, const uint8_t *data, size_t len)
{
    size_t sent = 0u;
    while (sent < len) {
        const ssize_t written = write(fd, data + sent, len - sent);
        if (written <= 0) {
            if (written < 0 && errno == EINTR)
                continue;
            return AA_TCP_ERR_IO;
        }
        sent += (size_t)written;
    }
    return AA_TCP_OK;
}

static aa_tcp_status drain(aa_session *session, int fd)
{
    for (;;) {
        const uint8_t *data = NULL;
        size_t len = 0u;
        if (!aa_session_peek(session, &data, &len))
            return AA_TCP_OK;
        const aa_tcp_status status = write_all(fd, data, len);
        if (status != AA_TCP_OK)
            return status;
        aa_session_consume(session);
    }
}

aa_tcp_status aa_tcp_serve(aa_tcp_listener *listener, aa_session *session, int accept_timeout_ms)
{
    if (listener == NULL || session == NULL || listener->listener < 0)
        return AA_TCP_ERR_ARGUMENT;

    struct timeval timeout;
    timeout.tv_sec = accept_timeout_ms / 1000;
    timeout.tv_usec = (accept_timeout_ms % 1000) * 1000;
    (void)setsockopt(listener->listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    const int peer = accept(listener->listener, NULL, NULL);
    if (peer < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? AA_TCP_ERR_TIMEOUT : AA_TCP_ERR_IO;

    aa_tcp_status result = AA_TCP_OK;
    uint8_t buffer[AA_TCP_IO_BUFFER];
    aa_session_out out;

    /* The head unit speaks first: whatever the session queued before the peer
     * arrived (the version request) has to leave before we read, or both sides
     * wait for each other. */
    const aa_tcp_status initial = drain(session, peer);
    if (initial != AA_TCP_OK) {
        (void)shutdown(peer, SHUT_RDWR);
        close(peer);
        return initial;
    }

    for (;;) {
        const ssize_t got = read(peer, buffer, sizeof buffer);
        if (got == 0) {
            result = AA_TCP_PEER_CLOSED;
            break;
        }
        if (got < 0) {
            if (errno == EINTR)
                continue;
            result = AA_TCP_ERR_IO;
            break;
        }

        const aa_session_status fed =
            aa_session_feed(session, buffer, (size_t)got, &out);
        const aa_tcp_status pumped = drain(session, peer);
        if (pumped != AA_TCP_OK) {
            result = pumped;
            break;
        }
        if (fed != AA_SESSION_OK) {
            result = AA_TCP_ERR_SESSION;
            break;
        }
        if (out.event == AA_CONTROL_EVENT_SHUTDOWN) {
            result = AA_TCP_OK;
            break;
        }
    }

    (void)shutdown(peer, SHUT_RDWR);
    close(peer);
    return result;
}

const char *aa_tcp_status_name(aa_tcp_status status)
{
    switch (status) {
    case AA_TCP_OK:
        return "ok";
    case AA_TCP_PEER_CLOSED:
        return "peer_closed";
    case AA_TCP_ERR_ARGUMENT:
        return "argument";
    case AA_TCP_ERR_IO:
        return "io";
    case AA_TCP_ERR_TIMEOUT:
        return "timeout";
    case AA_TCP_ERR_SESSION:
        return "session";
    }
    return "unknown";
}
