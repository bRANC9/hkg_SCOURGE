/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * TCP transport for the AA core (the wireless pipe).
 *
 * The engine under src/ owns no sockets on purpose: aa_session takes bytes in
 * and queues frames out.  This file is the other half — it owns the listening
 * socket, accepts one peer, and pumps bytes between the socket and the session.
 * A real TLS provider slots in unchanged, because the session already routes
 * every payload through the provider it was given.
 *
 * Which address and interface to bind is a deployment decision, not a protocol
 * one: on the bridge the AP's own address is the natural choice (that is the
 * endpoint the bootstrap announces over RFCOMM), and the firewall needs the
 * matching accept rule on the AP interface — the unit's CARPLAY_IN/OUT chains
 * with their DHCP exception are the precedent for that.
 *
 * Nothing here allocates, logs or reads configuration; failures are returned.
 */

#ifndef AA_TCP_H
#define AA_TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../src/aa_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AA_TCP_IO_BUFFER 4096u

typedef enum {
    AA_TCP_OK = 0,
    AA_TCP_PEER_CLOSED = 1,
    AA_TCP_ERR_ARGUMENT = -1,
    AA_TCP_ERR_IO = -2,
    AA_TCP_ERR_TIMEOUT = -3,
    AA_TCP_ERR_SESSION = -4
} aa_tcp_status;

typedef struct {
    int listener;
    uint16_t port;
} aa_tcp_listener;

/* Bind and listen.  bind_address may be NULL for loopback.  port 0 binds an
 * ephemeral port and reports it in listener->port. */
aa_tcp_status aa_tcp_open(aa_tcp_listener *listener, const char *bind_address, uint16_t port);

void aa_tcp_close(aa_tcp_listener *listener);

/* Accept one peer (waiting at most accept_timeout_ms) and pump the session over
 * it until the peer closes or the session fails.  Everything the session queues
 * is written back; a peer that stops reading is an I/O error, not a hang. */
aa_tcp_status aa_tcp_serve(aa_tcp_listener *listener, aa_session *session, int accept_timeout_ms);

const char *aa_tcp_status_name(aa_tcp_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_TCP_H */
