/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Control-channel state machine implementation.  See aa_control.h for the
 * message shapes, the advertised capability and the error policy.
 */
#include "aa_control.h"

#include <string.h>

#include "aa_proto.h"

#define AA_CONTROL_VERSION_STATUS_MATCH 0x0000u

static void write_u16_be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static aa_control_status done(aa_control *control, aa_control_out *out, aa_control_status status)
{
    out->state = control->state;
    return status;
}

void aa_control_out_reset(aa_control_out *out)
{
    if (out == NULL)
        return;
    out->payload_len = 0u;
    out->state = AA_CONTROL_IDLE;
    out->event = AA_CONTROL_EVENT_NONE;
}

void aa_control_reset(aa_control *control, uint16_t major, uint16_t minor)
{
    if (control == NULL)
        return;
    memset(control, 0, sizeof *control);
    control->state = AA_CONTROL_IDLE;
    control->major = major;
    control->minor = minor;
}

bool aa_control_advertises(uint8_t channel)
{
    return channel == AA_CONTROL_CHANNEL_SENSOR || channel == AA_CONTROL_CHANNEL_NAVIGATION ||
           channel == AA_CONTROL_CHANNEL_INPUT;
}

static aa_control_status begin_reply(aa_control_out *out, uint16_t message_id,
                                     aa_proto_writer *writer)
{
    aa_control_out_reset(out);
    write_u16_be(out->payload, message_id);
    aa_proto_writer_init(writer, out->payload + 2u, AA_CONTROL_MAX_PAYLOAD - 2u);
    return AA_CONTROL_OK;
}

static aa_control_status end_reply(aa_control_out *out, const aa_proto_writer *writer)
{
    if (!aa_proto_writer_ok(writer)) {
        out->payload_len = 0u;
        return AA_CONTROL_ERR_BUFFER;
    }
    out->payload_len = 2u + aa_proto_writer_len(writer);
    return AA_CONTROL_OK;
}

static aa_control_status reply_version_request(aa_control *control, aa_control_out *out)
{
    aa_control_out_reset(out);
    write_u16_be(out->payload, AA_CONTROL_VERSION_REQUEST);
    write_u16_be(out->payload + 2u, control->major);
    write_u16_be(out->payload + 4u, control->minor);
    out->payload_len = 6u;
    return AA_CONTROL_OK;
}

static aa_control_status reply_auth_complete(aa_control_out *out)
{
    aa_proto_writer writer;
    begin_reply(out, AA_CONTROL_AUTH_COMPLETE, &writer);
    if (aa_proto_put_varint_field(&writer, 1u, 0u) != AA_PROTO_OK)
        return end_reply(out, &writer);
    return end_reply(out, &writer);
}

/* Sensor types declared on the sensor channel: gear, car speed, parking brake,
 * driving status, night data and the EV energy model (23, undocumented in the
 * reference enum but the one the phone must see to plan EV routes).  More are a
 * one-line addition when a consumer exists. */
static const uint32_t kSensorTypes[] = {8u, 3u, 7u, 13u, 10u, 23u};

static aa_control_status reply_service_discovery(aa_control_out *out)
{
    aa_proto_writer writer;
    begin_reply(out, AA_CONTROL_SERVICE_DISCOVERY_RESPONSE, &writer);

    size_t descriptor = 0u;
    size_t nested = 0u;
    size_t inner = 0u;

    /* Sensor channel. */
    if (aa_proto_begin_message(&writer, 1u, &descriptor) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, AA_CONTROL_CHANNEL_SENSOR) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 2u, &nested) != AA_PROTO_OK)
        goto fail;
    for (size_t i = 0u; i < sizeof kSensorTypes / sizeof kSensorTypes[0]; i++) {
        if (aa_proto_begin_message(&writer, 1u, &inner) != AA_PROTO_OK)
            goto fail;
        if (aa_proto_put_varint_field(&writer, 1u, kSensorTypes[i]) != AA_PROTO_OK)
            goto fail;
        if (aa_proto_end_message(&writer, inner) != AA_PROTO_OK)
            goto fail;
    }
    if (aa_proto_end_message(&writer, nested) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, descriptor) != AA_PROTO_OK)
        goto fail;

    /* Navigation channel with a 480x480 icon request. */
    if (aa_proto_begin_message(&writer, 1u, &descriptor) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, AA_CONTROL_CHANNEL_NAVIGATION) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 8u, &nested) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, 500u) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 2u, 0u) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 3u, &inner) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, 480u) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 2u, 480u) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 3u, 32u) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, inner) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, nested) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, descriptor) != AA_PROTO_OK)
        goto fail;

    /* Input channel with a touch surface and no keycodes yet. */
    if (aa_proto_begin_message(&writer, 1u, &descriptor) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, AA_CONTROL_CHANNEL_INPUT) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 4u, &nested) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 2u, &inner) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, 1280u) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 2u, 720u) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, inner) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, nested) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, descriptor) != AA_PROTO_OK)
        goto fail;

    /* Head-unit identity.  Product names only. */
    if (aa_proto_put_string_field(&writer, 2u, AA_CONTROL_HEAD_UNIT_NAME) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_string_field(&writer, 3u, AA_CONTROL_CAR_MODEL) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_bool_field(&writer, 6u, true) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_string_field(&writer, 7u, AA_CONTROL_HEAD_UNIT_MANUFACTURER) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_string_field(&writer, 8u, AA_CONTROL_HEAD_UNIT_MODEL) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_string_field(&writer, 9u, AA_CONTROL_SW_BUILD) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_string_field(&writer, 10u, AA_CONTROL_SW_VERSION) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_bool_field(&writer, 12u, true) != AA_PROTO_OK)
        goto fail;

    return end_reply(out, &writer);

fail:
    return end_reply(out, &writer);
}

static aa_control_status reply_channel_open(aa_control_out *out, uint32_t status)
{
    aa_proto_writer writer;
    aa_control_status result = begin_reply(out, AA_CONTROL_CHANNEL_OPEN_RESPONSE, &writer);
    if (result != AA_CONTROL_OK)
        return result;
    (void)aa_proto_put_varint_field(&writer, 1u, status);
    return end_reply(out, &writer);
}

static aa_control_status reply_ping(aa_control_out *out, uint64_t timestamp)
{
    aa_proto_writer writer;
    aa_control_status result = begin_reply(out, AA_CONTROL_PING_RESPONSE, &writer);
    if (result != AA_CONTROL_OK)
        return result;
    (void)aa_proto_put_varint_field(&writer, 1u, timestamp);
    return end_reply(out, &writer);
}

static aa_control_status reply_shutdown(aa_control_out *out)
{
    aa_control_out_reset(out);
    write_u16_be(out->payload, AA_CONTROL_SHUTDOWN_RESPONSE);
    out->payload_len = 2u;
    return AA_CONTROL_OK;
}

static aa_control_status parse_version_response(const uint8_t *body, size_t body_len,
                                                uint16_t *major, uint16_t *minor, uint16_t *status)
{
    if (body_len != 6u)
        return AA_CONTROL_ERR_PARSE;
    *major = read_u16_be(body);
    *minor = read_u16_be(body + 2u);
    *status = read_u16_be(body + 4u);
    return AA_CONTROL_OK;
}

static aa_control_status parse_service_discovery_request(aa_control *control, const uint8_t *body,
                                                         size_t body_len)
{
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, body, body_len);

    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        aa_proto_status status = aa_proto_next(&iter, &field, &wire);
        if (status == AA_PROTO_END)
            return AA_CONTROL_OK;
        if (status != AA_PROTO_OK)
            return AA_CONTROL_ERR_PARSE;

        if (field == 4u && wire == AA_PROTO_WIRE_LEN) {
            char name[128];
            size_t name_len = 0u;
            if (aa_proto_read_string(&iter, name, sizeof name, &name_len) != AA_PROTO_OK)
                return AA_CONTROL_ERR_PARSE;
            if (name_len >= AA_CONTROL_MAX_DEVICE_NAME)
                name_len = AA_CONTROL_MAX_DEVICE_NAME - 1u;
            memcpy(control->device_name, name, name_len);
            control->device_name[name_len] = '\0';
            control->device_name_len = name_len;
            continue;
        }
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            return AA_CONTROL_ERR_PARSE;
    }
}

static aa_control_status parse_channel_open_request(const uint8_t *body, size_t body_len,
                                                    uint32_t *priority, uint32_t *channel_id)
{
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, body, body_len);

    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        aa_proto_status status = aa_proto_next(&iter, &field, &wire);
        if (status == AA_PROTO_END)
            return AA_CONTROL_OK;
        if (status != AA_PROTO_OK)
            return AA_CONTROL_ERR_PARSE;

        if ((field == 1u || field == 2u) && wire == AA_PROTO_WIRE_VARINT) {
            uint64_t value = 0u;
            if (aa_proto_read_varint(&iter, &value) != AA_PROTO_OK)
                return AA_CONTROL_ERR_PARSE;
            if (value > 0xFFFFFFFFu)
                return AA_CONTROL_ERR_PARSE;
            if (field == 1u)
                *priority = (uint32_t)value;
            else
                *channel_id = (uint32_t)value;
            continue;
        }
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            return AA_CONTROL_ERR_PARSE;
    }
}

static aa_control_status parse_ping(const uint8_t *body, size_t body_len, uint64_t *timestamp)
{
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, body, body_len);

    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        aa_proto_status status = aa_proto_next(&iter, &field, &wire);
        if (status == AA_PROTO_END)
            return AA_CONTROL_OK;
        if (status != AA_PROTO_OK)
            return AA_CONTROL_ERR_PARSE;

        if (field == 1u && wire == AA_PROTO_WIRE_VARINT) {
            if (aa_proto_read_varint(&iter, timestamp) != AA_PROTO_OK)
                return AA_CONTROL_ERR_PARSE;
            continue;
        }
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            return AA_CONTROL_ERR_PARSE;
    }
}

aa_control_status aa_control_start(aa_control *control, aa_control_out *out)
{
    if (control == NULL || out == NULL)
        return AA_CONTROL_ERR_ARGUMENT;
    aa_control_out_reset(out);
    if (control->state != AA_CONTROL_IDLE)
        return done(control, out, AA_CONTROL_ERR_STATE);

    aa_control_status status = reply_version_request(control, out);
    if (status != AA_CONTROL_OK) {
        control->errors++;
        return done(control, out, status);
    }
    control->state = AA_CONTROL_VERSION_SENT;
    control->replied++;
    return done(control, out, AA_CONTROL_OK);
}

aa_control_status aa_control_on_tls_established(aa_control *control, aa_control_out *out)
{
    if (control == NULL || out == NULL)
        return AA_CONTROL_ERR_ARGUMENT;
    aa_control_out_reset(out);
    if (control->state != AA_CONTROL_TLS_PENDING)
        return done(control, out, AA_CONTROL_ERR_STATE);

    const aa_control_status status = reply_auth_complete(out);
    if (status != AA_CONTROL_OK) {
        control->errors++;
        return done(control, out, status);
    }
    control->state = AA_CONTROL_AUTH_SENT;
    control->replied++;
    return done(control, out, AA_CONTROL_OK);
}

aa_control_status aa_control_on_message(aa_control *control, uint8_t channel, uint16_t message_id,
                                        const uint8_t *body, size_t body_len, aa_control_out *out)
{
    if (control == NULL || out == NULL)
        return AA_CONTROL_ERR_ARGUMENT;
    if (body == NULL && body_len != 0u)
        return AA_CONTROL_ERR_ARGUMENT;
    if (channel != (uint8_t)AA_CHANNEL_CONTROL) {
        control->errors++;
        return AA_CONTROL_ERR_CHANNEL;
    }

    aa_control_out_reset(out);
    out->state = control->state;

    switch (message_id) {
    case AA_CONTROL_VERSION_RESPONSE: {
        if (control->state != AA_CONTROL_VERSION_SENT)
            return done(control, out, AA_CONTROL_ERR_STATE);
        uint16_t peer_major = 0u;
        uint16_t peer_minor = 0u;
        uint16_t peer_status = 0u;
        if (parse_version_response(body, body_len, &peer_major, &peer_minor, &peer_status) !=
            AA_CONTROL_OK) {
            control->errors++;
            return done(control, out, AA_CONTROL_ERR_PARSE);
        }
        control->peer_major = peer_major;
        control->peer_minor = peer_minor;
        if (peer_status != AA_CONTROL_VERSION_STATUS_MATCH) {
            control->state = AA_CONTROL_FAILED;
            control->errors++;
            out->event = AA_CONTROL_EVENT_FAILED;
            return done(control, out, AA_CONTROL_ERR_VERSION);
        }
        control->version_matched = true;
        control->state = AA_CONTROL_TLS_PENDING;
        return done(control, out, AA_CONTROL_OK);
    }

    case AA_CONTROL_SERVICE_DISCOVERY_REQUEST: {
        if (control->state != AA_CONTROL_AUTH_SENT)
            return done(control, out, AA_CONTROL_ERR_STATE);
        if (parse_service_discovery_request(control, body, body_len) != AA_CONTROL_OK) {
            control->errors++;
            return done(control, out, AA_CONTROL_ERR_PARSE);
        }
        const aa_control_status status = reply_service_discovery(out);
        if (status != AA_CONTROL_OK) {
            control->errors++;
            return done(control, out, status);
        }
        control->replied++;
        control->state = AA_CONTROL_READY;
        out->event = AA_CONTROL_EVENT_READY;
        return done(control, out, AA_CONTROL_OK);
    }

    case AA_CONTROL_CHANNEL_OPEN_REQUEST: {
        if (control->state != AA_CONTROL_READY)
            return done(control, out, AA_CONTROL_ERR_STATE);
        uint32_t priority = 0u;
        uint32_t channel_id = 0u;
        if (parse_channel_open_request(body, body_len, &priority, &channel_id) != AA_CONTROL_OK) {
            control->errors++;
            return done(control, out, AA_CONTROL_ERR_PARSE);
        }
        const bool accepted = channel_id <= 0xFFu && aa_control_advertises((uint8_t)channel_id);
        const aa_control_status status = reply_channel_open(out, accepted ? 0u : 1u);
        if (status != AA_CONTROL_OK) {
            control->errors++;
            return done(control, out, status);
        }
        if (accepted) {
            control->opened_mask |= 1u << channel_id;
            control->replied++;
        } else {
            control->ignored++;
        }
        return done(control, out, AA_CONTROL_OK);
    }

    case AA_CONTROL_PING_REQUEST: {
        if (control->state != AA_CONTROL_AUTH_SENT && control->state != AA_CONTROL_READY)
            return done(control, out, AA_CONTROL_ERR_STATE);
        uint64_t timestamp = 0u;
        if (parse_ping(body, body_len, &timestamp) != AA_CONTROL_OK) {
            control->errors++;
            return done(control, out, AA_CONTROL_ERR_PARSE);
        }
        const aa_control_status status = reply_ping(out, timestamp);
        if (status != AA_CONTROL_OK) {
            control->errors++;
            return done(control, out, status);
        }
        control->replied++;
        return done(control, out, AA_CONTROL_OK);
    }

    case AA_CONTROL_PING_RESPONSE:
        control->ignored++;
        return done(control, out, AA_CONTROL_OK);

    case AA_CONTROL_SHUTDOWN_REQUEST: {
        if (control->state != AA_CONTROL_AUTH_SENT && control->state != AA_CONTROL_READY)
            return done(control, out, AA_CONTROL_ERR_STATE);
        const aa_control_status status = reply_shutdown(out);
        if (status != AA_CONTROL_OK) {
            control->errors++;
            return done(control, out, status);
        }
        control->replied++;
        control->state = AA_CONTROL_SHUTDOWN;
        out->event = AA_CONTROL_EVENT_SHUTDOWN;
        return done(control, out, AA_CONTROL_OK);
    }

    default:
        control->ignored++;
        return done(control, out, AA_CONTROL_OK);
    }
}

const char *aa_control_state_name(aa_control_state state)
{
    switch (state) {
    case AA_CONTROL_IDLE:
        return "idle";
    case AA_CONTROL_VERSION_SENT:
        return "version_sent";
    case AA_CONTROL_TLS_PENDING:
        return "tls_pending";
    case AA_CONTROL_AUTH_SENT:
        return "auth_sent";
    case AA_CONTROL_READY:
        return "ready";
    case AA_CONTROL_SHUTDOWN:
        return "shutdown";
    case AA_CONTROL_FAILED:
        return "failed";
    }
    return "unknown";
}

const char *aa_control_status_name(aa_control_status status)
{
    switch (status) {
    case AA_CONTROL_OK:
        return "ok";
    case AA_CONTROL_ERR_ARGUMENT:
        return "argument";
    case AA_CONTROL_ERR_CHANNEL:
        return "channel";
    case AA_CONTROL_ERR_STATE:
        return "state";
    case AA_CONTROL_ERR_MESSAGE_ID:
        return "message_id";
    case AA_CONTROL_ERR_PARSE:
        return "parse";
    case AA_CONTROL_ERR_BUFFER:
        return "buffer";
    case AA_CONTROL_ERR_VERSION:
        return "version";
    }
    return "unknown";
}
