/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Harness for the Python transport test: listens on loopback, serves exactly one
 * peer through aa_tcp + aa_session with the deterministic stand-in provider, and
 * prints the resulting session state so the test can assert it.
 *
 * The stand-in XORs every byte with 0x5A.  It models record framing and
 * reversibility only, never confidentiality, and it must never be installed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aa_session.h"
#include "../src/aa_tls.h"
#include "../transport/aa_tcp.h"

typedef struct {
    uint8_t pending[512];
    size_t pending_len;
    uint8_t incoming[512];
    size_t incoming_len;
    int phase;
} stand_in;

static aa_tls_status stand_in_reset(void *context)
{
    stand_in *stand = context;
    stand->pending_len = 0u;
    stand->incoming_len = 0u;
    stand->phase = 0;
    return AA_TLS_OK;
}

static aa_tls_status stand_in_handshake(void *context)
{
    stand_in *stand = context;
    if (stand->phase == 0) {
        memcpy(stand->pending, "HELLO1", 6u);
        stand->pending_len = 6u;
        stand->phase = 1;
        return AA_TLS_OK;
    }
    if (stand->phase == 1 && stand->incoming_len != 0u) {
        memcpy(stand->pending, "FINISHED", 8u);
        stand->pending_len = 8u;
        stand->incoming_len = 0u;
        stand->phase = 2;
        return AA_TLS_ESTABLISHED;
    }
    stand->pending_len = 0u;
    return AA_TLS_OK;
}

static aa_tls_status stand_in_feed(void *context, const uint8_t *data, size_t len)
{
    stand_in *stand = context;
    if (len > sizeof stand->incoming)
        return AA_TLS_ERR_BUFFER;
    memcpy(stand->incoming, data, len);
    stand->incoming_len = len;
    return AA_TLS_OK;
}

static aa_tls_status stand_in_take(void *context, uint8_t *out, size_t out_cap, size_t *out_len)
{
    stand_in *stand = context;
    if (stand->pending_len > out_cap) {
        *out_len = 0u;
        return AA_TLS_ERR_BUFFER;
    }
    memcpy(out, stand->pending, stand->pending_len);
    *out_len = stand->pending_len;
    stand->pending_len = 0u;
    return AA_TLS_OK;
}

static aa_tls_status stand_in_write(void *context, const uint8_t *data, size_t len)
{
    stand_in *stand = context;
    if (len > sizeof stand->pending)
        return AA_TLS_ERR_BUFFER;
    memcpy(stand->pending, data, len);
    for (size_t i = 0u; i < len; i++)
        stand->pending[i] = (uint8_t)(stand->pending[i] ^ 0x5Au);
    stand->pending_len = len;
    return AA_TLS_OK;
}

static aa_tls_status stand_in_read(void *context, uint8_t *out, size_t out_cap, size_t *out_len)
{
    stand_in *stand = context;
    if (stand->incoming_len > out_cap) {
        *out_len = 0u;
        return AA_TLS_ERR_BUFFER;
    }
    memcpy(out, stand->incoming, stand->incoming_len);
    for (size_t i = 0u; i < stand->incoming_len; i++)
        out[i] = (uint8_t)(out[i] ^ 0x5Au);
    *out_len = stand->incoming_len;
    stand->incoming_len = 0u;
    return AA_TLS_OK;
}

int main(int argc, char **argv)
{
    unsigned port = 0u;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (unsigned)strtoul(argv[++i], NULL, 10);
    }

    static stand_in stand;
    memset(&stand, 0, sizeof stand);
    aa_tls_provider provider;
    provider.context = &stand;
    provider.reset = stand_in_reset;
    provider.handshake = stand_in_handshake;
    provider.feed = stand_in_feed;
    provider.take = stand_in_take;
    provider.write = stand_in_write;
    provider.read = stand_in_read;

    static aa_session session;
    aa_session_reset(&session, &provider, 1u, 0u);
    aa_session_out out;
    if (aa_session_start(&session, &out) != AA_SESSION_OK) {
        (void)fprintf(stderr, "session start failed\n");
        return 1;
    }

    aa_tcp_listener listener;
    if (aa_tcp_open(&listener, NULL, (uint16_t)port) != AA_TCP_OK) {
        (void)fprintf(stderr, "listen failed\n");
        return 1;
    }
    (void)printf("port=%u\n", (unsigned)listener.port);
    (void)fflush(stdout);

    const aa_tcp_status served = aa_tcp_serve(&listener, &session, 10000);
    aa_tcp_close(&listener);

    /* One line the test can assert on: how far the session got. */
    (void)printf("served=%s state=%s starts=%u ev=%s\n", aa_tcp_status_name(served),
                 aa_session_state_name(session.state), session.sensor.start_requests,
                 session.sensor.ev_energy_requested ? "yes" : "no");
    (void)fflush(stdout);
    return served == AA_TCP_PEER_CLOSED || served == AA_TCP_OK ? 0 : 1;
}
