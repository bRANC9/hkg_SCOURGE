/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Session glue implementation.  See aa_session.h for the ordering and the
 * local policy.
 */
#include "aa_session.h"

#include <string.h>

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_u16_be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static aa_session_state map_state(aa_control_state state)
{
    switch (state) {
    case AA_CONTROL_IDLE:
        return AA_SESSION_IDLE;
    case AA_CONTROL_VERSION_SENT:
        return AA_SESSION_VERSION_SENT;
    case AA_CONTROL_TLS_PENDING:
        return AA_SESSION_HANDSHAKE;
    case AA_CONTROL_AUTH_SENT:
        return AA_SESSION_AUTH_SENT;
    case AA_CONTROL_READY:
        return AA_SESSION_READY;
    case AA_CONTROL_SHUTDOWN:
        return AA_SESSION_SHUTDOWN;
    case AA_CONTROL_FAILED:
        return AA_SESSION_FAILED;
    }
    return AA_SESSION_FAILED;
}

static aa_session_status fail(aa_session *session, aa_session_status status)
{
    session->state = AA_SESSION_FAILED;
    return status;
}

static aa_session_status queue_channel_frame(aa_session *session, uint8_t channel,
                                            const uint8_t *payload, size_t payload_len,
                                            bool encrypted)
{
    if (session->queue_count >= AA_SESSION_QUEUE_DEPTH)
        return AA_SESSION_ERR_QUEUE;
    const size_t index = (session->queue_head + session->queue_count) % AA_SESSION_QUEUE_DEPTH;
    aa_session_frame *frame = &session->queue[index];
    size_t frame_len = 0u;
    if (aa_wire_encode_message(channel, false, encrypted, payload, payload_len,
                               AA_WIRE_MAX_FRAME_PAYLOAD, frame->data, sizeof frame->data,
                               &frame_len) != AA_WIRE_OK)
        return AA_SESSION_ERR_QUEUE;
    frame->len = frame_len;
    session->queue_count++;
    return AA_SESSION_OK;
}

/* Encrypted outbound payload: everything outside the control handshake goes
 * through the secure session first, then onto the queue as a flagged frame. */
static aa_session_status queue_encrypted(aa_session *session, uint8_t channel,
                                         const uint8_t *payload, size_t payload_len)
{
    size_t cypher_len = 0u;
    const aa_tls_status tls_status = aa_tls_encrypt(&session->tls, payload, payload_len,
                                                    session->cypher, sizeof session->cypher,
                                                    &cypher_len);
    if (tls_status != AA_TLS_OK)
        return fail(session, AA_SESSION_ERR_TLS);
    const aa_session_status status =
        queue_channel_frame(session, channel, session->cypher, cypher_len, true);
    return status == AA_SESSION_OK ? AA_SESSION_OK : fail(session, status);
}

/* Control replies that travel encrypted, per the reference behaviour.  The
 * decision belongs to the reply, not to the request that triggered it. */
static bool control_reply_encrypted(uint16_t message_id)
{
    switch (message_id) {
    case AA_CONTROL_SERVICE_DISCOVERY_RESPONSE:
    case AA_CONTROL_CHANNEL_OPEN_RESPONSE:
    case AA_CONTROL_PING_RESPONSE:
    case AA_CONTROL_SHUTDOWN_RESPONSE:
        return true;
    default:
        return false;
    }
}

/* After a TLS step: queue whatever the provider produced, and if the session
 * just came up, send auth complete and mark the handshake finished. */
static aa_session_status after_tls_step(aa_session *session, size_t handshake_len,
                                        aa_session_out *out)
{
    if (handshake_len != 0u) {
        /* The handshake body is already staged at cypher + 2. */
        write_u16_be(session->cypher, AA_CONTROL_SSL_HANDSHAKE);
        const aa_session_status status = queue_channel_frame(
            session, (uint8_t)AA_CHANNEL_CONTROL, session->cypher, handshake_len + 2u, false);
        if (status != AA_SESSION_OK)
            return status;
    }
    if (aa_tls_is_established(&session->tls)) {
        aa_control_out reply;
        const aa_control_status control_status =
            aa_control_on_tls_established(&session->control, &reply);
        if (control_status != AA_CONTROL_OK)
            return fail(session, AA_SESSION_ERR_CONTROL);
        if (reply.payload_len != 0u) {
            const aa_session_status status = queue_channel_frame(
                session, (uint8_t)AA_CHANNEL_CONTROL, reply.payload, reply.payload_len, false);
            if (status != AA_SESSION_OK)
                return status;
        }
        if (reply.event != AA_CONTROL_EVENT_NONE)
            out->event = reply.event;
    }
    return AA_SESSION_OK;
}

static aa_session_status start_handshake(aa_session *session, aa_session_out *out)
{
    if (session->provider == NULL)
        return fail(session, AA_SESSION_ERR_TLS);
    size_t handshake_len = 0u;
    const aa_tls_status tls_status =
        aa_tls_begin(&session->tls, session->provider, session->cypher + 2u,
                     AA_TLS_MAX_HANDSHAKE_BLOB, &handshake_len);
    if (tls_status != AA_TLS_OK)
        return fail(session, AA_SESSION_ERR_TLS);
    return after_tls_step(session, handshake_len, out);
}

static aa_session_status handle_control(aa_session *session, const aa_wire_message *message,
                                        aa_session_out *out)
{
    const uint8_t *payload = message->payload;
    size_t payload_len = message->payload_len;

    if (message->encrypted) {
        size_t plain_len = 0u;
        const aa_tls_status tls_status =
            aa_tls_decrypt(&session->tls, message->payload, message->payload_len, session->scratch,
                           sizeof session->scratch, &plain_len);
        if (tls_status != AA_TLS_OK)
            return fail(session, AA_SESSION_ERR_TLS);
        payload = session->scratch;
        payload_len = plain_len;
    }

    if (payload_len < 2u)
        return fail(session, AA_SESSION_ERR_PROTOCOL);

    const uint16_t message_id = read_u16_be(payload);
    const uint8_t *body = payload + 2u;
    const size_t body_len = payload_len - 2u;

    if (message_id == AA_CONTROL_SSL_HANDSHAKE) {
        if (message->encrypted)
            return fail(session, AA_SESSION_ERR_PROTOCOL);
        size_t handshake_len = 0u;
        const aa_tls_status tls_status =
            aa_tls_on_handshake(&session->tls, body, body_len, session->cypher + 2u,
                                AA_TLS_MAX_HANDSHAKE_BLOB, &handshake_len);
        if (tls_status != AA_TLS_OK)
            return fail(session, AA_SESSION_ERR_TLS);
        return after_tls_step(session, handshake_len, out);
    }

    aa_control_out reply;
    const aa_control_status control_status =
        aa_control_on_message(&session->control, message->channel, message_id, body, body_len,
                              &reply);
    if (control_status == AA_CONTROL_ERR_VERSION) {
        out->event = AA_CONTROL_EVENT_FAILED;
        return fail(session, AA_SESSION_ERR_CONTROL);
    }
    if (control_status != AA_CONTROL_OK)
        return fail(session, AA_SESSION_ERR_CONTROL);
    if (reply.event != AA_CONTROL_EVENT_NONE)
        out->event = reply.event;

    if (reply.payload_len != 0u) {
        const bool encrypt_reply =
            reply.payload_len >= 2u && control_reply_encrypted(read_u16_be(reply.payload));
        if (encrypt_reply) {
            size_t cypher_len = 0u;
            const aa_tls_status tls_status =
                aa_tls_encrypt(&session->tls, reply.payload, reply.payload_len, session->cypher,
                               sizeof session->cypher, &cypher_len);
            if (tls_status != AA_TLS_OK)
                return fail(session, AA_SESSION_ERR_TLS);
            const aa_session_status status = queue_channel_frame(
                session, (uint8_t)AA_CHANNEL_CONTROL, session->cypher, cypher_len, true);
            if (status != AA_SESSION_OK)
                return fail(session, status);
        } else {
            const aa_session_status status = queue_channel_frame(
                session, (uint8_t)AA_CHANNEL_CONTROL, reply.payload, reply.payload_len, false);
            if (status != AA_SESSION_OK)
                return fail(session, status);
        }
    }

    /* A matching version response moves the protocol to the secure session. */
    if (session->control.state == AA_CONTROL_TLS_PENDING && session->tls.state == AA_TLS_IDLE) {
        const aa_session_status status = start_handshake(session, out);
        if (status != AA_SESSION_OK)
            return fail(session, status);
    }
    return AA_SESSION_OK;
}

/* The sensor channel repeats the open handshake on its own channel, then asks
 * for individual sensors; everything it receives travels encrypted. */
static aa_session_status handle_sensor(aa_session *session, const aa_wire_message *message)
{
    const uint8_t *payload = message->payload;
    size_t payload_len = message->payload_len;

    if (message->encrypted) {
        size_t plain_len = 0u;
        const aa_tls_status tls_status =
            aa_tls_decrypt(&session->tls, message->payload, message->payload_len, session->scratch,
                           sizeof session->scratch, &plain_len);
        if (tls_status != AA_TLS_OK)
            return fail(session, AA_SESSION_ERR_TLS);
        payload = session->scratch;
        payload_len = plain_len;
    }
    if (payload_len < 2u)
        return fail(session, AA_SESSION_ERR_PROTOCOL);

    const uint16_t message_id = read_u16_be(payload);
    aa_sensor_out reply;
    const aa_sensor_status sensor_status =
        aa_sensor_on_message(&session->sensor, message_id, payload + 2u, payload_len - 2u, &reply);
    if (sensor_status != AA_SENSOR_OK)
        return fail(session, AA_SESSION_ERR_PROTOCOL);
    if (reply.payload_len == 0u)
        return AA_SESSION_OK;

    return queue_encrypted(session, (uint8_t)AA_CHANNEL_SENSOR, reply.payload, reply.payload_len);
}

size_t aa_session_pending(const aa_session *session)
{
    return session == NULL ? 0u : session->queue_count;
}

bool aa_session_peek(const aa_session *session, const uint8_t **data, size_t *len)
{
    if (session == NULL || data == NULL || len == NULL || session->queue_count == 0u)
        return false;
    *data = session->queue[session->queue_head].data;
    *len = session->queue[session->queue_head].len;
    return true;
}

void aa_session_consume(aa_session *session)
{
    if (session == NULL || session->queue_count == 0u)
        return;
    session->queue[session->queue_head].len = 0u;
    session->queue_head = (session->queue_head + 1u) % AA_SESSION_QUEUE_DEPTH;
    session->queue_count--;
}

aa_session_status aa_session_reset(aa_session *session, const aa_tls_provider *provider,
                                   uint16_t major, uint16_t minor)
{
    if (session == NULL)
        return AA_SESSION_ERR_ARGUMENT;
    memset(session, 0, sizeof *session);
    session->provider = provider;
    aa_control_reset(&session->control, major, minor);
    aa_tls_init(&session->tls);
    aa_wire_decode_init(&session->decode);
    session->state = AA_SESSION_IDLE;
    return AA_SESSION_OK;
}

aa_session_status aa_session_start(aa_session *session, aa_session_out *out)
{
    if (session == NULL || out == NULL)
        return AA_SESSION_ERR_ARGUMENT;
    out->consumed = 0u;
    out->event = AA_CONTROL_EVENT_NONE;
    out->state = session->state;

    if (session->control.state != AA_CONTROL_IDLE)
        return AA_SESSION_ERR_STATE;

    aa_control_out reply;
    const aa_control_status control_status = aa_control_start(&session->control, &reply);
    if (control_status != AA_CONTROL_OK)
        return fail(session, AA_SESSION_ERR_CONTROL);
    const aa_session_status status = queue_channel_frame(
        session, (uint8_t)AA_CHANNEL_CONTROL, reply.payload, reply.payload_len, false);
    if (status != AA_SESSION_OK)
        return fail(session, status);

    session->state = map_state(session->control.state);
    out->state = session->state;
    return AA_SESSION_OK;
}

aa_session_status aa_session_set_vehicle(aa_session *session, const aa_sensor_vehicle *vehicle)
{
    if (session == NULL || vehicle == NULL)
        return AA_SESSION_ERR_ARGUMENT;
    session->vehicle = *vehicle;
    if (!session->sensor.ev_energy_requested)
        return AA_SESSION_OK;

    uint8_t payload[AA_SENSOR_MAX_PAYLOAD];
    size_t payload_len = 0u;
    const aa_sensor_status sensor_status =
        aa_sensor_encode_ev_energy(vehicle, payload, sizeof payload, &payload_len);
    if (sensor_status == AA_SENSOR_ERR_RANGE)
        return AA_SESSION_OK; /* nothing trustworthy to report */
    if (sensor_status != AA_SENSOR_OK)
        return fail(session, AA_SESSION_ERR_PROTOCOL);

    return queue_encrypted(session, (uint8_t)AA_CHANNEL_SENSOR, payload, payload_len);
}

aa_session_status aa_session_send_sensor_value(aa_session *session, aa_sensor_value kind,
                                               int32_t raw)
{
    if (session == NULL)
        return AA_SESSION_ERR_ARGUMENT;
    if (!session->sensor.started)
        return AA_SESSION_ERR_STATE;

    uint8_t payload[AA_SENSOR_MAX_PAYLOAD];
    size_t payload_len = 0u;
    const aa_sensor_status sensor_status =
        aa_sensor_encode_value(kind, raw, payload, sizeof payload, &payload_len);
    if (sensor_status != AA_SENSOR_OK)
        return AA_SESSION_ERR_PROTOCOL;

    return queue_encrypted(session, (uint8_t)AA_CHANNEL_SENSOR, payload, payload_len);
}

aa_session_status aa_session_feed(aa_session *session, const uint8_t *data, size_t len,
                                  aa_session_out *out)
{
    if (session == NULL || out == NULL)
        return AA_SESSION_ERR_ARGUMENT;
    if (data == NULL && len != 0u)
        return AA_SESSION_ERR_ARGUMENT;

    out->consumed = 0u;
    out->event = AA_CONTROL_EVENT_NONE;
    out->state = session->state;

    while (out->consumed < len) {
        aa_wire_message message;
        size_t consumed = 0u;
        const aa_wire_status wire_status =
            aa_wire_decode_message(&session->decode, data + out->consumed, len - out->consumed,
                                   session->message, sizeof session->message, &message, &consumed);
        if (wire_status == AA_WIRE_END || wire_status == AA_WIRE_ERR_TRUNCATED) {
            out->consumed += consumed;
            break;
        }
        if (wire_status != AA_WIRE_OK) {
            out->state = AA_SESSION_FAILED;
            return fail(session, AA_SESSION_ERR_WIRE);
        }
        out->consumed += consumed;

        aa_session_status status = AA_SESSION_OK;
        if (message.channel == (uint8_t)AA_CHANNEL_CONTROL) {
            status = handle_control(session, &message, out);
        } else if (message.channel == (uint8_t)AA_CHANNEL_SENSOR) {
            status = handle_sensor(session, &message);
        } else {
            /* Channels this checkpoint does not implement are counted, not
             * fatal: the phone may open video or audio at any time. */
            session->ignored_channels++;
        }
        if (status != AA_SESSION_OK) {
            out->state = AA_SESSION_FAILED;
            return status;
        }
    }

    session->state = map_state(session->control.state);
    out->state = session->state;
    return AA_SESSION_OK;
}

const char *aa_session_state_name(aa_session_state state)
{
    switch (state) {
    case AA_SESSION_IDLE:
        return "idle";
    case AA_SESSION_VERSION_SENT:
        return "version_sent";
    case AA_SESSION_HANDSHAKE:
        return "handshake";
    case AA_SESSION_AUTH_SENT:
        return "auth_sent";
    case AA_SESSION_READY:
        return "ready";
    case AA_SESSION_SHUTDOWN:
        return "shutdown";
    case AA_SESSION_FAILED:
        return "failed";
    }
    return "unknown";
}

const char *aa_session_status_name(aa_session_status status)
{
    switch (status) {
    case AA_SESSION_OK:
        return "ok";
    case AA_SESSION_ERR_ARGUMENT:
        return "argument";
    case AA_SESSION_ERR_STATE:
        return "state";
    case AA_SESSION_ERR_PROTOCOL:
        return "protocol";
    case AA_SESSION_ERR_CONTROL:
        return "control";
    case AA_SESSION_ERR_TLS:
        return "tls";
    case AA_SESSION_ERR_QUEUE:
        return "queue";
    case AA_SESSION_ERR_WIRE:
        return "wire";
    }
    return "unknown";
}
