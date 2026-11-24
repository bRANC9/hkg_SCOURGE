/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Frame-layer codec implementation.  See aa_wire.h for the wire facts, the
 * bounds and the streaming contract.  No allocation, no I/O, no logging.
 */
#include "aa_wire.h"

#include <string.h>

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)((value >> 16) & 0xFFu);
    p[2] = (uint8_t)((value >> 8) & 0xFFu);
    p[3] = (uint8_t)(value & 0xFFu);
}

static bool frame_type_is_first(uint8_t flags)
{
    /* Only exactly FIRST uses the extended size field; BULK (FIRST|LAST) uses
     * the short one, exactly as the reference frame parser masks the bits. */
    return (flags & AA_WIRE_FLAG_FRAME_TYPE_MASK) == AA_WIRE_FLAG_FIRST;
}

static bool flags_known(uint8_t flags)
{
    return (flags & (uint8_t)~AA_WIRE_FLAG_MASK) == 0u;
}

aa_wire_status aa_wire_parse_frame(const uint8_t *buf, size_t len, aa_wire_frame *out)
{
    if (buf == NULL || out == NULL)
        return AA_WIRE_ERR_ARGUMENT;
    if (len < AA_WIRE_HEADER_LEN)
        return AA_WIRE_ERR_TRUNCATED;

    const uint8_t channel = buf[0];
    const uint8_t flags = buf[1];
    if (!flags_known(flags))
        return AA_WIRE_ERR_FLAGS;

    const bool first = frame_type_is_first(flags);
    const bool last = (flags & AA_WIRE_FLAG_LAST) != 0u;
    const size_t size_len = first ? AA_WIRE_SIZE_EXTENDED_LEN : AA_WIRE_SIZE_SHORT_LEN;

    if (len < AA_WIRE_HEADER_LEN + size_len)
        return AA_WIRE_ERR_TRUNCATED;

    const uint16_t payload_len = read_be16(buf + AA_WIRE_HEADER_LEN);
    if (payload_len > AA_WIRE_MAX_FRAME_PAYLOAD)
        return AA_WIRE_ERR_OVERSIZE;

    const size_t frame_len = AA_WIRE_HEADER_LEN + size_len + (size_t)payload_len;
    if (len < frame_len)
        return AA_WIRE_ERR_TRUNCATED;

    uint32_t total_len = 0u;
    if (first) {
        total_len = read_be32(buf + AA_WIRE_HEADER_LEN + AA_WIRE_SIZE_SHORT_LEN);
        if (total_len == 0u || total_len < (uint32_t)payload_len || total_len > AA_WIRE_MAX_MESSAGE)
            return AA_WIRE_ERR_LENGTH;
    }

    out->channel = channel;
    out->flags = flags;
    out->control = (flags & AA_WIRE_FLAG_CONTROL) != 0u;
    out->encrypted = (flags & AA_WIRE_FLAG_ENCRYPTED) != 0u;
    out->first = first;
    out->last = last;
    out->payload = buf + AA_WIRE_HEADER_LEN + size_len;
    out->payload_len = (size_t)payload_len;
    out->total_len = total_len;
    out->frame_len = frame_len;
    return AA_WIRE_OK;
}

aa_wire_status aa_wire_encode_frame(uint8_t channel, uint8_t flags, uint32_t total_len,
                                    const uint8_t *payload, size_t payload_len,
                                    uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out == NULL || out_len == NULL)
        return AA_WIRE_ERR_ARGUMENT;
    if (!flags_known(flags))
        return AA_WIRE_ERR_FLAGS;
    if (payload == NULL && payload_len != 0u)
        return AA_WIRE_ERR_ARGUMENT;
    if (payload_len > AA_WIRE_MAX_FRAME_PAYLOAD)
        return AA_WIRE_ERR_OVERSIZE;

    const bool first = frame_type_is_first(flags);
    const size_t size_len = first ? AA_WIRE_SIZE_EXTENDED_LEN : AA_WIRE_SIZE_SHORT_LEN;
    const size_t frame_len = AA_WIRE_HEADER_LEN + size_len + payload_len;

    if (first) {
        if (total_len == 0u || total_len < (uint32_t)payload_len || total_len > AA_WIRE_MAX_MESSAGE)
            return AA_WIRE_ERR_LENGTH;
    } else if (total_len != 0u) {
        return AA_WIRE_ERR_LENGTH;
    }

    if (out_cap < frame_len)
        return AA_WIRE_ERR_OVERSIZE;

    out[0] = channel;
    out[1] = flags;
    write_be16(out + AA_WIRE_HEADER_LEN, (uint16_t)payload_len);
    if (first)
        write_be32(out + AA_WIRE_HEADER_LEN + AA_WIRE_SIZE_SHORT_LEN, total_len);
    if (payload_len != 0u)
        memcpy(out + AA_WIRE_HEADER_LEN + size_len, payload, payload_len);

    *out_len = frame_len;
    return AA_WIRE_OK;
}

aa_wire_status aa_wire_encode_message(uint8_t channel, bool control, bool encrypted,
                                      const uint8_t *payload, size_t payload_len,
                                      size_t chunk, uint8_t *out, size_t out_cap,
                                      size_t *out_len)
{
    if (out == NULL || out_len == NULL)
        return AA_WIRE_ERR_ARGUMENT;
    if (payload == NULL && payload_len != 0u)
        return AA_WIRE_ERR_ARGUMENT;
    if (chunk == 0u || chunk > AA_WIRE_MAX_FRAME_PAYLOAD)
        return AA_WIRE_ERR_ARGUMENT;
    if (payload_len > AA_WIRE_MAX_MESSAGE)
        return AA_WIRE_ERR_OVERSIZE;

    const uint8_t base_flags = (uint8_t)((control ? AA_WIRE_FLAG_CONTROL : 0u) |
                                         (encrypted ? AA_WIRE_FLAG_ENCRYPTED : 0u));

    if (payload_len <= chunk) {
        return aa_wire_encode_frame(channel, (uint8_t)(base_flags | AA_WIRE_FLAG_BULK), 0u,
                                    payload, payload_len, out, out_cap, out_len);
    }

    size_t written = 0u;
    size_t sent = 0u;
    while (sent < payload_len) {
        size_t part = payload_len - sent;
        if (part > chunk)
            part = chunk;
        const bool is_last_part = (sent + part) == payload_len;
        uint8_t flags;
        if (sent == 0u)
            flags = (uint8_t)(base_flags | AA_WIRE_FLAG_FIRST);
        else if (is_last_part)
            flags = (uint8_t)(base_flags | AA_WIRE_FLAG_LAST);
        else
            flags = base_flags;

        size_t frame_len = 0u;
        const aa_wire_status status = aa_wire_encode_frame(
            channel, flags, sent == 0u ? (uint32_t)payload_len : 0u,
            payload + sent, part, out + written, out_cap - written, &frame_len);
        if (status != AA_WIRE_OK)
            return status;
        written += frame_len;
        sent += part;
    }

    *out_len = written;
    return AA_WIRE_OK;
}

void aa_wire_decode_init(aa_wire_decode *state)
{
    if (state == NULL)
        return;
    state->active = false;
    state->channel = 0u;
    state->control = false;
    state->encrypted = false;
    state->total = 0u;
    state->received = 0u;
}

aa_wire_status aa_wire_decode_message(aa_wire_decode *state, const uint8_t *buf, size_t len,
                                      uint8_t *msg, size_t msg_cap,
                                      aa_wire_message *out, size_t *consumed)
{
    if (state == NULL || buf == NULL || msg == NULL || out == NULL || consumed == NULL)
        return AA_WIRE_ERR_ARGUMENT;
    *consumed = 0u;

    size_t offset = 0u;
    for (;;) {
        if (len - offset < AA_WIRE_HEADER_LEN) {
            *consumed = offset;
            return offset == 0u ? AA_WIRE_ERR_TRUNCATED : AA_WIRE_END;
        }

        aa_wire_frame frame;
        aa_wire_status status = aa_wire_parse_frame(buf + offset, len - offset, &frame);
        if (status == AA_WIRE_ERR_TRUNCATED) {
            *consumed = offset;
            return offset == 0u ? AA_WIRE_ERR_TRUNCATED : AA_WIRE_END;
        }
        if (status != AA_WIRE_OK) {
            aa_wire_decode_init(state);
            return status;
        }

        if (!state->active) {
            if (!frame.first && !frame.last) {
                aa_wire_decode_init(state);
                return AA_WIRE_ERR_FLAGS;
            }
            if (frame.last && !frame.first) {
                /* BULK: this frame is the whole message. */
                if (frame.payload_len > msg_cap) {
                    aa_wire_decode_init(state);
                    return AA_WIRE_ERR_OVERSIZE;
                }
                if (frame.payload_len != 0u)
                    memcpy(msg, frame.payload, frame.payload_len);
                out->channel = frame.channel;
                out->control = frame.control;
                out->encrypted = frame.encrypted;
                out->payload = msg;
                out->payload_len = frame.payload_len;
                *consumed = offset + frame.frame_len;
                aa_wire_decode_init(state);
                return AA_WIRE_OK;
            }
            if (frame.total_len > msg_cap) {
                aa_wire_decode_init(state);
                return AA_WIRE_ERR_OVERSIZE;
            }
            state->active = true;
            state->channel = frame.channel;
            state->control = frame.control;
            state->encrypted = frame.encrypted;
            state->total = frame.total_len;
            state->received = 0u;
        } else {
            if (frame.channel != state->channel || frame.control != state->control ||
                frame.encrypted != state->encrypted) {
                aa_wire_decode_init(state);
                return AA_WIRE_ERR_INTERLEAVED;
            }
            if (frame.first) {
                aa_wire_decode_init(state);
                return AA_WIRE_ERR_FLAGS;
            }
            if (state->received == (size_t)state->total && !(frame.last && frame.payload_len == 0u)) {
                aa_wire_decode_init(state);
                return AA_WIRE_ERR_LENGTH;
            }
        }

        if (state->received + frame.payload_len > (size_t)state->total) {
            aa_wire_decode_init(state);
            return AA_WIRE_ERR_LENGTH;
        }
        if (frame.payload_len != 0u)
            memcpy(msg + state->received, frame.payload, frame.payload_len);
        state->received += frame.payload_len;
        offset += frame.frame_len;

        if (frame.last) {
            if (state->received != (size_t)state->total) {
                aa_wire_decode_init(state);
                return AA_WIRE_ERR_LENGTH;
            }
            out->channel = state->channel;
            out->control = state->control;
            out->encrypted = state->encrypted;
            out->payload = msg;
            out->payload_len = state->received;
            *consumed = offset;
            aa_wire_decode_init(state);
            return AA_WIRE_OK;
        }
    }
}

const char *aa_wire_status_name(aa_wire_status status)
{
    switch (status) {
    case AA_WIRE_OK:
        return "ok";
    case AA_WIRE_END:
        return "end";
    case AA_WIRE_ERR_ARGUMENT:
        return "argument";
    case AA_WIRE_ERR_TRUNCATED:
        return "truncated";
    case AA_WIRE_ERR_FLAGS:
        return "flags";
    case AA_WIRE_ERR_LENGTH:
        return "length";
    case AA_WIRE_ERR_CHANNEL:
        return "channel";
    case AA_WIRE_ERR_INTERLEAVED:
        return "interleaved";
    case AA_WIRE_ERR_OVERSIZE:
        return "oversize";
    }
    return "unknown";
}
