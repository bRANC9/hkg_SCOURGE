/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Phone-role mirror implementation.  See aa_mirror.h for the conversation, the
 * pinned facts and the local policy.
 */
#include "aa_mirror.h"

#include <string.h>

#include "aa_proto.h"
#include "aa_sensor.h"

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static aa_mirror_status begin_out(aa_mirror_out *out, uint16_t message_id, uint8_t channel,
                                  bool encrypted)
{
    if (out->payload_len != 0u)
        return AA_MIRROR_ERR_BUFFER; /* one message per call */
    write_be16(out->payload, message_id);
    out->payload_len = 2u;
    out->channel = channel;
    out->encrypted = encrypted;
    return AA_MIRROR_OK;
}

static aa_mirror_status finish_out(aa_mirror_out *out, const aa_proto_writer *writer)
{
    if (!aa_proto_writer_ok(writer)) {
        out->payload_len = 0u;
        return AA_MIRROR_ERR_BUFFER;
    }
    out->payload_len = 2u + aa_proto_writer_len(writer);
    return AA_MIRROR_OK;
}

static bool channel_wanted(const aa_mirror *mirror, uint8_t channel)
{
    return channel <= 31u && (mirror->wanted_mask & (1u << channel)) != 0u;
}

static bool channel_opened(const aa_mirror *mirror, uint8_t channel)
{
    return channel <= 31u && (mirror->opened_mask & (1u << channel)) != 0u;
}

static aa_mirror_status emit_channel_open(aa_mirror *mirror, uint8_t channel, aa_mirror_out *out)
{
    const aa_mirror_status began = begin_out(out, AA_CONTROL_CHANNEL_OPEN_REQUEST, AA_CHANNEL_CONTROL,
                                             true);
    if (began != AA_MIRROR_OK)
        return began;
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, out->payload + 2u, AA_MIRROR_MAX_PAYLOAD - 2u);
    (void)aa_proto_put_varint_field(&writer, 1u, 0u); /* priority */
    (void)aa_proto_put_varint_field(&writer, 2u, channel);
    (void)mirror;
    return finish_out(out, &writer);
}

static aa_mirror_status emit_sensor_start(aa_mirror_out *out)
{
    const aa_mirror_status began = begin_out(out, AA_SENSOR_START_REQUEST, (uint8_t)AA_CHANNEL_SENSOR,
                                             true);
    if (began != AA_MIRROR_OK)
        return began;
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, out->payload + 2u, AA_MIRROR_MAX_PAYLOAD - 2u);
    (void)aa_proto_put_varint_field(&writer, 1u, AA_SENSOR_EV_ENERGY_MODEL_TYPE);
    (void)aa_proto_put_varint_field(&writer, 2u, AA_MIRROR_SENSOR_INTERVAL_MS);
    return finish_out(out, &writer);
}

/* The next request the mirror owes the head unit, if any. */
static aa_mirror_status emit_next_request(aa_mirror *mirror, aa_mirror_out *out)
{
    for (uint8_t channel = 0u; channel < 32u; channel++) {
        if (channel_wanted(mirror, channel) && !channel_opened(mirror, channel) &&
            (mirror->refused_mask & (1u << channel)) == 0u)
            return emit_channel_open(mirror, channel, out);
    }
    if (channel_opened(mirror, (uint8_t)AA_CHANNEL_SENSOR) && !mirror->sensor_started)
        return emit_sensor_start(out);
    return AA_MIRROR_OK;
}

static aa_mirror_status parse_version_request(const uint8_t *body, size_t body_len,
                                              uint16_t *major, uint16_t *minor)
{
    if (body_len != 4u)
        return AA_MIRROR_ERR_PARSE;
    *major = read_be16(body);
    *minor = read_be16(body + 2u);
    return AA_MIRROR_OK;
}

static aa_mirror_status parse_discovery_response(aa_mirror *mirror, const uint8_t *body,
                                                 size_t body_len)
{
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, body, body_len);
    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        const aa_proto_status proto_status = aa_proto_next(&iter, &field, &wire);
        if (proto_status == AA_PROTO_END)
            return AA_MIRROR_OK;
        if (proto_status != AA_PROTO_OK)
            return AA_MIRROR_ERR_PARSE;

        if (field == 1u && wire == AA_PROTO_WIRE_LEN) {
            const uint8_t *descriptor = NULL;
            size_t descriptor_len = 0u;
            if (aa_proto_read_len(&iter, &descriptor, &descriptor_len) != AA_PROTO_OK)
                return AA_MIRROR_ERR_PARSE;
            aa_proto_iter sub;
            aa_proto_iter_init(&sub, descriptor, descriptor_len);
            uint32_t sub_field = 0u;
            uint8_t sub_wire = 0u;
            if (aa_proto_next(&sub, &sub_field, &sub_wire) == AA_PROTO_OK && sub_field == 1u &&
                sub_wire == AA_PROTO_WIRE_VARINT) {
                uint64_t channel_id = 0u;
                if (aa_proto_read_varint(&sub, &channel_id) == AA_PROTO_OK && channel_id < 32u)
                    mirror->advertised_mask |= 1u << channel_id;
            }
            continue;
        }
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            return AA_MIRROR_ERR_PARSE;
    }
}

static aa_mirror_status parse_open_response(const uint8_t *body, size_t body_len, uint32_t *status)
{
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, body, body_len);
    bool have_status = false;
    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        const aa_proto_status proto_status = aa_proto_next(&iter, &field, &wire);
        if (proto_status == AA_PROTO_END)
            break;
        if (proto_status != AA_PROTO_OK)
            return AA_MIRROR_ERR_PARSE;
        if (field == 1u && wire == AA_PROTO_WIRE_VARINT) {
            uint64_t value = 0u;
            if (aa_proto_read_varint(&iter, &value) != AA_PROTO_OK)
                return AA_MIRROR_ERR_PARSE;
            *status = (uint32_t)value;
            have_status = true;
            continue;
        }
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            return AA_MIRROR_ERR_PARSE;
    }
    return have_status ? AA_MIRROR_OK : AA_MIRROR_ERR_PARSE;
}

/* Only as deep as the report needs: the current and the maximum energy. */
static void parse_ev_model(aa_mirror *mirror, const uint8_t *body, size_t body_len)
{
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, body, body_len);
    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        if (aa_proto_next(&iter, &field, &wire) != AA_PROTO_OK)
            return;
        if (field != AA_SENSOR_EV_ENERGY_MODEL_TYPE || wire != AA_PROTO_WIRE_LEN) {
            if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
                return;
            continue;
        }

        const uint8_t *model = NULL;
        size_t model_len = 0u;
        if (aa_proto_read_len(&iter, &model, &model_len) != AA_PROTO_OK)
            return;
        mirror->ev_seen = true;

        aa_proto_iter model_iter;
        aa_proto_iter_init(&model_iter, model, model_len);
        for (;;) {
            uint32_t model_field = 0u;
            uint8_t model_wire = 0u;
            if (aa_proto_next(&model_iter, &model_field, &model_wire) != AA_PROTO_OK)
                return;
            if (model_field != 1u || model_wire != AA_PROTO_WIRE_LEN) {
                if (aa_proto_skip(&model_iter, model_wire) != AA_PROTO_OK)
                    return;
                continue;
            }
            const uint8_t *battery = NULL;
            size_t battery_len = 0u;
            if (aa_proto_read_len(&model_iter, &battery, &battery_len) != AA_PROTO_OK)
                return;
            aa_proto_iter battery_iter;
            aa_proto_iter_init(&battery_iter, battery, battery_len);
            for (;;) {
                uint32_t battery_field = 0u;
                uint8_t battery_wire = 0u;
                if (aa_proto_next(&battery_iter, &battery_field, &battery_wire) != AA_PROTO_OK)
                    return;
                if ((battery_field == 3u || battery_field == 4u) &&
                    battery_wire == AA_PROTO_WIRE_LEN) {
                    const uint8_t *energy = NULL;
                    size_t energy_len = 0u;
                    if (aa_proto_read_len(&battery_iter, &energy, &energy_len) != AA_PROTO_OK)
                        return;
                    aa_proto_iter energy_iter;
                    aa_proto_iter_init(&energy_iter, energy, energy_len);
                    uint32_t energy_field = 0u;
                    uint8_t energy_wire = 0u;
                    uint64_t watt_hours = 0u;
                    if (aa_proto_next(&energy_iter, &energy_field, &energy_wire) == AA_PROTO_OK &&
                        energy_field == 1u && energy_wire == AA_PROTO_WIRE_VARINT &&
                        aa_proto_read_varint(&energy_iter, &watt_hours) == AA_PROTO_OK) {
                        if (battery_field == 3u)
                            mirror->ev_current_wh = (int32_t)watt_hours;
                        else
                            mirror->ev_capacity_wh = (int32_t)watt_hours;
                    }
                    continue;
                }
                if (aa_proto_skip(&battery_iter, battery_wire) != AA_PROTO_OK)
                    return;
            }
        }
    }
}

void aa_mirror_reset(aa_mirror *mirror, uint16_t major, uint16_t minor)
{
    if (mirror == NULL)
        return;
    memset(mirror, 0, sizeof *mirror);
    mirror->state = AA_MIRROR_IDLE;
    mirror->version_major = major;
    mirror->version_minor = minor;
    mirror->ev_current_wh = -1;
    mirror->ev_capacity_wh = -1;
}

void aa_mirror_set_secure(aa_mirror *mirror, bool secure)
{
    if (mirror == NULL)
        return;
    mirror->secure = secure;
}

aa_mirror_status aa_mirror_on_message(aa_mirror *mirror, uint8_t channel, uint16_t message_id,
                                      const uint8_t *body, size_t body_len, aa_mirror_out *out)
{
    if (mirror == NULL || out == NULL)
        return AA_MIRROR_ERR_ARGUMENT;
    if (body == NULL && body_len != 0u)
        return AA_MIRROR_ERR_ARGUMENT;

    out->payload_len = 0u;
    out->channel = channel;
    out->encrypted = false;
    out->event = AA_MIRROR_EVENT_NONE;
    out->state = mirror->state;

    /* Ping travels either way at any time after the version exchange. */
    if (message_id == AA_CONTROL_PING_REQUEST) {
        uint64_t timestamp = 0u;
        aa_proto_iter iter;
        aa_proto_iter_init(&iter, body, body_len);
        for (;;) {
            uint32_t field = 0u;
            uint8_t wire = 0u;
            if (aa_proto_next(&iter, &field, &wire) != AA_PROTO_OK)
                break;
            if (field == 1u && wire == AA_PROTO_WIRE_VARINT) {
                (void)aa_proto_read_varint(&iter, &timestamp);
                continue;
            }
            if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
                break;
        }
        const aa_mirror_status began = begin_out(out, AA_CONTROL_PING_RESPONSE, AA_CHANNEL_CONTROL,
                                                 true);
        if (began != AA_MIRROR_OK)
            return began;
        aa_proto_writer writer;
        aa_proto_writer_init(&writer, out->payload + 2u, AA_MIRROR_MAX_PAYLOAD - 2u);
        (void)aa_proto_put_varint_field(&writer, 1u, timestamp);
        return finish_out(out, &writer);
    }

    if (channel != (uint8_t)AA_CHANNEL_CONTROL) {
        if (channel == (uint8_t)AA_CHANNEL_SENSOR && message_id == AA_SENSOR_EVENT_INDICATION) {
            mirror->sensor_events++;
            if (!mirror->ev_seen)
                parse_ev_model(mirror, body, body_len);
            if (mirror->ev_seen)
                out->event = AA_MIRROR_EVENT_EV_MODEL;
            out->state = mirror->state;
            return AA_MIRROR_OK;
        }
        mirror->ignored++;
        return AA_MIRROR_OK;
    }

    switch (message_id) {
    case AA_CONTROL_VERSION_REQUEST: {
        uint16_t major = 0u;
        uint16_t minor = 0u;
        if (parse_version_request(body, body_len, &major, &minor) != AA_MIRROR_OK) {
            mirror->errors++;
            return AA_MIRROR_ERR_PARSE;
        }
        mirror->hu_major = major;
        mirror->hu_minor = minor;
        if (mirror->state == AA_MIRROR_IDLE)
            mirror->state = AA_MIRROR_VERSION_ANSWERED;
        out->state = mirror->state;

        const aa_mirror_status began = begin_out(out, AA_CONTROL_VERSION_RESPONSE,
                                                 AA_CHANNEL_CONTROL, false);
        if (began != AA_MIRROR_OK)
            return began;
        aa_proto_writer writer;
        aa_proto_writer_init(&writer, out->payload + 2u, AA_MIRROR_MAX_PAYLOAD - 2u);
        (void)aa_proto_put_varint_field(&writer, 1u, mirror->version_major);
        (void)aa_proto_put_varint_field(&writer, 2u, mirror->version_minor);
        (void)aa_proto_put_varint_field(&writer, 3u, 0u); /* MATCH */
        return finish_out(out, &writer);
    }

    case AA_CONTROL_AUTH_COMPLETE: {
        if (mirror->state != AA_MIRROR_VERSION_ANSWERED || !mirror->secure)
            return AA_MIRROR_ERR_STATE;
        const aa_mirror_status began = begin_out(out, AA_CONTROL_SERVICE_DISCOVERY_REQUEST,
                                                 AA_CHANNEL_CONTROL, true);
        if (began != AA_MIRROR_OK)
            return began;
        aa_proto_writer writer;
        aa_proto_writer_init(&writer, out->payload + 2u, AA_MIRROR_MAX_PAYLOAD - 2u);
        (void)aa_proto_put_string_field(&writer, 4u, AA_MIRROR_DEVICE_NAME);
        (void)aa_proto_put_string_field(&writer, 5u, AA_MIRROR_DEVICE_BRAND);
        const aa_mirror_status finished = finish_out(out, &writer);
        if (finished != AA_MIRROR_OK)
            return finished;
        mirror->state = AA_MIRROR_DISCOVERY_SENT;
        out->state = mirror->state;
        return AA_MIRROR_OK;
    }

    case AA_CONTROL_SERVICE_DISCOVERY_RESPONSE: {
        if (mirror->state != AA_MIRROR_DISCOVERY_SENT)
            return AA_MIRROR_ERR_STATE;
        if (parse_discovery_response(mirror, body, body_len) != AA_MIRROR_OK) {
            mirror->errors++;
            return AA_MIRROR_ERR_PARSE;
        }
        mirror->wanted_mask = mirror->advertised_mask &
                              ((1u << (uint8_t)AA_CHANNEL_SENSOR) |
                               (1u << (uint8_t)AA_CHANNEL_NAVIGATION));
        mirror->state = AA_MIRROR_READY;
        out->state = mirror->state;
        const aa_mirror_status next = emit_next_request(mirror, out);
        if (next != AA_MIRROR_OK)
            return next;
        if (out->payload_len == 0u)
            out->event = AA_MIRROR_EVENT_READY;
        return AA_MIRROR_OK;
    }

    case AA_CONTROL_CHANNEL_OPEN_RESPONSE: {
        if (mirror->state != AA_MIRROR_READY)
            return AA_MIRROR_ERR_STATE;
        uint32_t status = 1u;
        if (parse_open_response(body, body_len, &status) != AA_MIRROR_OK) {
            mirror->errors++;
            return AA_MIRROR_ERR_PARSE;
        }
        /* Which channel this answers is the one we asked about last: the caller
         * feeds answers in order, which the one-request-at-a-time policy makes
         * unambiguous. */
        uint8_t pending = 0u;
        for (uint8_t c = 0u; c < 32u; c++) {
            if (channel_wanted(mirror, c) && !channel_opened(mirror, c) &&
                (mirror->refused_mask & (1u << c)) == 0u) {
                pending = c;
                break;
            }
        }
        if (status == 0u)
            mirror->opened_mask |= 1u << pending;
        else
            mirror->refused_mask |= 1u << pending;

        aa_mirror_status next = emit_next_request(mirror, out);
        if (next != AA_MIRROR_OK)
            return next;
        if (out->payload_len != 0u) {
            if (out->payload[0] == (uint8_t)(AA_SENSOR_START_REQUEST >> 8) &&
                out->payload[1] == (uint8_t)(AA_SENSOR_START_REQUEST & 0xFFu))
                mirror->sensor_started = true;
            return AA_MIRROR_OK;
        }
        out->event = AA_MIRROR_EVENT_READY;
        return AA_MIRROR_OK;
    }

    default:
        mirror->ignored++;
        return AA_MIRROR_OK;
    }
}

const char *aa_mirror_state_name(aa_mirror_state state)
{
    switch (state) {
    case AA_MIRROR_IDLE:
        return "idle";
    case AA_MIRROR_VERSION_ANSWERED:
        return "version_answered";
    case AA_MIRROR_DISCOVERY_SENT:
        return "discovery_sent";
    case AA_MIRROR_READY:
        return "ready";
    case AA_MIRROR_FAILED:
        return "failed";
    }
    return "unknown";
}

const char *aa_mirror_status_name(aa_mirror_status status)
{
    switch (status) {
    case AA_MIRROR_OK:
        return "ok";
    case AA_MIRROR_ERR_ARGUMENT:
        return "argument";
    case AA_MIRROR_ERR_STATE:
        return "state";
    case AA_MIRROR_ERR_CHANNEL:
        return "channel";
    case AA_MIRROR_ERR_MESSAGE_ID:
        return "message_id";
    case AA_MIRROR_ERR_PARSE:
        return "parse";
    case AA_MIRROR_ERR_BUFFER:
        return "buffer";
    case AA_MIRROR_ERR_VERSION:
        return "version";
    }
    return "unknown";
}
