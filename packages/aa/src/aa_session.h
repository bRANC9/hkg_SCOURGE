/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Session glue of the AA core: frames in, frames out, protocol in between.
 *
 * The session owns one conversation and nothing else.  It decodes frames with
 * aa_wire, decrypts encrypted channel payloads through aa_tls, hands control
 * messages to aa_control, and queues the encoded replies for a transport to
 * pick up.  It never touches a socket, a clock or a device: the same object
 * serves the USB transport and the wireless one.
 *
 * Ordering implemented here:
 *   start                    -> control version request (plain)
 *   version response match   -> TLS handshake starts: SSL_HANDSHAKE (plain)
 *   TLS established          -> AUTH_COMPLETE (plain)
 *   discovery request        -> discovery response (encrypted)
 *   channel open / ping /
 *   shutdown                 -> respective reply (encrypted)
 *   sensor channel open      -> channel open response on the sensor channel
 *   sensor start request     -> sensor start response (encrypted)
 *   vehicle state pushed     -> sensor event indication with the EV energy
 *                               model, but only if the phone requested type 23
 *
 * Encryption follows the reference behaviour: every non-control channel
 * message is encrypted, and on the control channel only version, handshake and
 * auth-complete travel in the clear.  An encrypted frame is decrypted *before*
 * its two-byte message ID is read, because the ID is inside the encrypted
 * payload.
 *
 * Local policy, not wire facts: at AA_SESSION_MAX_PAYLOAD a payload is rejected,
 * the output queue holds AA_SESSION_QUEUE_DEPTH frames and a full queue fails
 * the session instead of dropping a reply, and any protocol, TLS or wire error
 * is terminal for the session.  Decoding a partial trailing frame consumes
 * nothing, so a transport may hand over arbitrary chunks.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_SESSION_H
#define AA_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aa_channels.h"
#include "aa_control.h"
#include "aa_sensor.h"
#include "aa_tls.h"
#include "aa_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AA_SESSION_MAX_PAYLOAD 4096u
#define AA_SESSION_MAX_FRAME \
    (AA_WIRE_HEADER_LEN + AA_WIRE_SIZE_EXTENDED_LEN + AA_SESSION_MAX_PAYLOAD + 64u)
#define AA_SESSION_QUEUE_DEPTH 4u

typedef enum {
    AA_SESSION_OK = 0,
    AA_SESSION_ERR_ARGUMENT = -1,
    AA_SESSION_ERR_STATE = -2,
    AA_SESSION_ERR_PROTOCOL = -3,
    AA_SESSION_ERR_CONTROL = -4,
    AA_SESSION_ERR_TLS = -5,
    AA_SESSION_ERR_QUEUE = -6,
    AA_SESSION_ERR_WIRE = -7
} aa_session_status;

typedef enum {
    AA_SESSION_IDLE = 0,
    AA_SESSION_VERSION_SENT = 1,
    AA_SESSION_HANDSHAKE = 2,
    AA_SESSION_AUTH_SENT = 3,
    AA_SESSION_READY = 4,
    AA_SESSION_SHUTDOWN = 5,
    AA_SESSION_FAILED = 6
} aa_session_state;

typedef struct {
    uint8_t data[AA_SESSION_MAX_FRAME];
    size_t len;
} aa_session_frame;

typedef struct {
    /* Bytes of this call's input that formed complete frames; the transport
     * keeps the remainder. */
    size_t consumed;
    aa_session_state state;
    aa_control_event event;
} aa_session_out;

typedef struct {
    const aa_tls_provider *provider;
    aa_control control;
    aa_sensor sensor;
    aa_sensor_vehicle vehicle;
    aa_tls tls;
    aa_wire_decode decode;
    aa_session_state state;
    aa_session_frame queue[AA_SESSION_QUEUE_DEPTH];
    size_t queue_head;
    size_t queue_count;
    uint8_t message[AA_SESSION_MAX_PAYLOAD];
    uint8_t scratch[AA_SESSION_MAX_PAYLOAD];
    uint8_t cypher[AA_SESSION_MAX_FRAME];
    uint32_t ignored_channels;
} aa_session;

/* Prepare a fresh session.  provider may be NULL only when no handshake is
 * expected, which no real transport should do. */
aa_session_status aa_session_reset(aa_session *session, const aa_tls_provider *provider,
                                   uint16_t major, uint16_t minor);

/* IDLE -> version request queued. */
aa_session_status aa_session_start(aa_session *session, aa_session_out *out);

/* Consume incoming bytes, queue outgoing frames. */
aa_session_status aa_session_feed(aa_session *session, const uint8_t *data, size_t len,
                                  aa_session_out *out);

/* Push the current vehicle state.  When the phone has requested sensor type 23
 * this queues an EV energy event; when the state is not valid or out of range
 * nothing is queued, because a fabricated state of charge is worse than none. */
aa_session_status aa_session_set_vehicle(aa_session *session, const aa_sensor_vehicle *vehicle);

/* Queue one simple sensor value (gear, driving status, night mode, parking
 * brake, speed).  Requires that the phone started the sensor channel. */
aa_session_status aa_session_send_sensor_value(aa_session *session, aa_sensor_value kind,
                                              int32_t raw);

/* Peek the oldest queued frame; consume it once it has been sent. */
bool aa_session_peek(const aa_session *session, const uint8_t **data, size_t *len);
void aa_session_consume(aa_session *session);
size_t aa_session_pending(const aa_session *session);

const char *aa_session_state_name(aa_session_state state);
const char *aa_session_status_name(aa_session_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_SESSION_H */
