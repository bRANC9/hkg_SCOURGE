/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Protobuf wire-format codec.  See aa_proto.h for the policy and the bounds.
 */
#include "aa_proto.h"

#include <string.h>

static bool read_raw_varint(const uint8_t *data, size_t len, size_t *offset, uint64_t *value,
                           bool *truncated)
{
    uint64_t result = 0u;
    size_t used = 0u;
    *truncated = false;
    for (;;) {
        if (*offset + used >= len) {
            *truncated = true;
            return false;
        }
        const uint8_t byte = data[*offset + used];
        if (used == AA_PROTO_MAX_VARINT_BYTES - 1u) {
            /* Tenth byte may only contribute the last bit. */
            if (byte > 0x01u)
                return false;
        }
        result |= (uint64_t)(byte & 0x7Fu) << (7u * used);
        used++;
        if ((byte & 0x80u) == 0u)
            break;
        if (used == AA_PROTO_MAX_VARINT_BYTES)
            return false;
    }
    *offset += used;
    *value = result;
    return true;
}

void aa_proto_iter_init(aa_proto_iter *iter, const uint8_t *data, size_t len)
{
    if (iter == NULL)
        return;
    iter->data = data;
    iter->len = data == NULL ? 0u : len;
    iter->offset = 0u;
}

aa_proto_status aa_proto_next(aa_proto_iter *iter, uint32_t *field_number, uint8_t *wire_type)
{
    if (iter == NULL || field_number == NULL || wire_type == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    if (iter->offset >= iter->len)
        return AA_PROTO_END;

    uint64_t key = 0u;
    bool truncated = false;
    if (!read_raw_varint(iter->data, iter->len, &iter->offset, &key, &truncated))
        return truncated ? AA_PROTO_ERR_TRUNCATED : AA_PROTO_ERR_VARINT;

    const uint64_t field = key >> 3;
    const uint8_t wire = (uint8_t)(key & 0x07u);
    if (field == 0u || field > 0x1FFFFFFFu)
        return AA_PROTO_ERR_WIRE;
    if (wire != AA_PROTO_WIRE_VARINT && wire != AA_PROTO_WIRE_FIXED64 && wire != AA_PROTO_WIRE_LEN &&
        wire != AA_PROTO_WIRE_FIXED32)
        return AA_PROTO_ERR_WIRE;

    *field_number = (uint32_t)field;
    *wire_type = wire;
    return AA_PROTO_OK;
}

aa_proto_status aa_proto_read_varint(aa_proto_iter *iter, uint64_t *value)
{
    if (iter == NULL || value == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    bool truncated = false;
    if (!read_raw_varint(iter->data, iter->len, &iter->offset, value, &truncated))
        return truncated ? AA_PROTO_ERR_TRUNCATED : AA_PROTO_ERR_VARINT;
    return AA_PROTO_OK;
}

static aa_proto_status read_length(aa_proto_iter *iter, size_t *length)
{
    uint64_t raw = 0u;
    const aa_proto_status status = aa_proto_read_varint(iter, &raw);
    if (status != AA_PROTO_OK)
        return status;
    if (raw > (uint64_t)(iter->len - iter->offset))
        return AA_PROTO_ERR_TRUNCATED;
    *length = (size_t)raw;
    return AA_PROTO_OK;
}

aa_proto_status aa_proto_read_fixed32(aa_proto_iter *iter, uint32_t *value)
{
    if (iter == NULL || value == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    if (iter->len - iter->offset < 4u)
        return AA_PROTO_ERR_TRUNCATED;
    const uint8_t *p = iter->data + iter->offset;
    *value = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
             ((uint32_t)p[3] << 24);
    iter->offset += 4u;
    return AA_PROTO_OK;
}

aa_proto_status aa_proto_read_fixed64(aa_proto_iter *iter, uint64_t *value)
{
    if (iter == NULL || value == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    if (iter->len - iter->offset < 8u)
        return AA_PROTO_ERR_TRUNCATED;
    const uint8_t *p = iter->data + iter->offset;
    uint64_t result = 0u;
    for (size_t i = 0u; i < 8u; i++)
        result |= (uint64_t)p[i] << (8u * i);
    *value = result;
    iter->offset += 8u;
    return AA_PROTO_OK;
}

aa_proto_status aa_proto_read_len(aa_proto_iter *iter, const uint8_t **data, size_t *len)
{
    if (iter == NULL || data == NULL || len == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    size_t length = 0u;
    const aa_proto_status status = read_length(iter, &length);
    if (status != AA_PROTO_OK)
        return status;
    *data = iter->data + iter->offset;
    *len = length;
    iter->offset += length;
    return AA_PROTO_OK;
}

bool aa_proto_utf8_valid(const char *text, size_t len)
{
    if (text == NULL)
        return false;
    size_t i = 0u;
    while (i < len) {
        const uint8_t byte = (uint8_t)text[i];
        if (byte == 0u)
            return false; /* embedded NUL cannot round-trip as a string */
        size_t extra = 0u;
        uint32_t codepoint = 0u;
        if (byte < 0x80u) {
            i++;
            continue;
        } else if ((byte & 0xE0u) == 0xC0u) {
            extra = 1u;
            codepoint = byte & 0x1Fu;
        } else if ((byte & 0xF0u) == 0xE0u) {
            extra = 2u;
            codepoint = byte & 0x0Fu;
        } else if ((byte & 0xF8u) == 0xF0u) {
            extra = 3u;
            codepoint = byte & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= len)
            return false;
        for (size_t k = 1u; k <= extra; k++) {
            const uint8_t cont = (uint8_t)text[i + k];
            if ((cont & 0xC0u) != 0x80u)
                return false;
            codepoint = (codepoint << 6) | (uint32_t)(cont & 0x3Fu);
        }
        /* Reject overlong encodings, surrogates and out-of-range values. */
        if ((extra == 1u && codepoint < 0x80u) || (extra == 2u && codepoint < 0x800u) ||
            (extra == 3u && codepoint < 0x10000u))
            return false;
        if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
            return false;
        i += extra + 1u;
    }
    return true;
}

aa_proto_status aa_proto_read_string(aa_proto_iter *iter, char *out, size_t out_cap,
                                     size_t *out_len)
{
    if (iter == NULL || out == NULL || out_len == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    const uint8_t *data = NULL;
    size_t length = 0u;
    aa_proto_status status = aa_proto_read_len(iter, &data, &length);
    if (status != AA_PROTO_OK)
        return status;
    if (length + 1u > out_cap)
        return AA_PROTO_ERR_OVERFLOW;
    if (!aa_proto_utf8_valid((const char *)data, length))
        return AA_PROTO_ERR_UTF8;
    memcpy(out, data, length);
    out[length] = '\0';
    *out_len = length;
    return AA_PROTO_OK;
}

aa_proto_status aa_proto_skip(aa_proto_iter *iter, uint8_t wire_type)
{
    if (iter == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    switch (wire_type) {
    case AA_PROTO_WIRE_VARINT: {
        uint64_t ignored = 0u;
        return aa_proto_read_varint(iter, &ignored);
    }
    case AA_PROTO_WIRE_FIXED64:
        if (iter->len - iter->offset < 8u)
            return AA_PROTO_ERR_TRUNCATED;
        iter->offset += 8u;
        return AA_PROTO_OK;
    case AA_PROTO_WIRE_LEN: {
        const uint8_t *ignored_data = NULL;
        size_t ignored_len = 0u;
        return aa_proto_read_len(iter, &ignored_data, &ignored_len);
    }
    case AA_PROTO_WIRE_FIXED32:
        if (iter->len - iter->offset < 4u)
            return AA_PROTO_ERR_TRUNCATED;
        iter->offset += 4u;
        return AA_PROTO_OK;
    }
    return AA_PROTO_ERR_WIRE;
}

const char *aa_proto_status_name(aa_proto_status status)
{
    switch (status) {
    case AA_PROTO_OK:
        return "ok";
    case AA_PROTO_END:
        return "end";
    case AA_PROTO_ERR_ARGUMENT:
        return "argument";
    case AA_PROTO_ERR_VARINT:
        return "varint";
    case AA_PROTO_ERR_WIRE:
        return "wire";
    case AA_PROTO_ERR_TRUNCATED:
        return "truncated";
    case AA_PROTO_ERR_OVERFLOW:
        return "overflow";
    case AA_PROTO_ERR_UTF8:
        return "utf8";
    }
    return "unknown";
}

void aa_proto_writer_init(aa_proto_writer *writer, uint8_t *buf, size_t cap)
{
    if (writer == NULL)
        return;
    writer->buf = buf;
    writer->cap = buf == NULL ? 0u : cap;
    writer->len = 0u;
    writer->failed = buf == NULL;
}

bool aa_proto_writer_ok(const aa_proto_writer *writer)
{
    return writer != NULL && !writer->failed;
}

size_t aa_proto_writer_len(const aa_proto_writer *writer)
{
    return writer == NULL ? 0u : writer->len;
}

static bool put_raw_varint(aa_proto_writer *writer, uint64_t value)
{
    uint8_t encoded[AA_PROTO_MAX_VARINT_BYTES];
    size_t used = 0u;
    do {
        uint8_t byte = (uint8_t)(value & 0x7Fu);
        value >>= 7;
        if (value != 0u)
            byte |= 0x80u;
        encoded[used++] = byte;
    } while (value != 0u);

    if (writer->cap - writer->len < used) {
        writer->failed = true;
        return false;
    }
    memcpy(writer->buf + writer->len, encoded, used);
    writer->len += used;
    return true;
}

static bool put_raw_bytes(aa_proto_writer *writer, const uint8_t *data, size_t len)
{
    if (writer->cap - writer->len < len) {
        writer->failed = true;
        return false;
    }
    if (len != 0u)
        memcpy(writer->buf + writer->len, data, len);
    writer->len += len;
    return true;
}

static aa_proto_status put_key(aa_proto_writer *writer, uint32_t field, uint8_t wire)
{
    if (field == 0u || field > 0x1FFFFFFFu)
        return AA_PROTO_ERR_WIRE;
    if (!put_raw_varint(writer, ((uint64_t)field << 3) | (uint64_t)wire))
        return AA_PROTO_ERR_OVERFLOW;
    return AA_PROTO_OK;
}

aa_proto_status aa_proto_put_varint_field(aa_proto_writer *writer, uint32_t field, uint64_t value)
{
    if (writer == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    if (writer->failed)
        return AA_PROTO_ERR_OVERFLOW;
    aa_proto_status status = put_key(writer, field, AA_PROTO_WIRE_VARINT);
    if (status != AA_PROTO_OK)
        return status;
    return put_raw_varint(writer, value) ? AA_PROTO_OK : AA_PROTO_ERR_OVERFLOW;
}

aa_proto_status aa_proto_put_bool_field(aa_proto_writer *writer, uint32_t field, bool value)
{
    return aa_proto_put_varint_field(writer, field, value ? 1u : 0u);
}

aa_proto_status aa_proto_put_float_field(aa_proto_writer *writer, uint32_t field, float value)
{
    if (writer == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    if (writer->failed)
        return AA_PROTO_ERR_OVERFLOW;
    const aa_proto_status status = put_key(writer, field, AA_PROTO_WIRE_FIXED32);
    if (status != AA_PROTO_OK)
        return status;

    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof bits);
    uint8_t little[4];
    little[0] = (uint8_t)(bits & 0xFFu);
    little[1] = (uint8_t)((bits >> 8) & 0xFFu);
    little[2] = (uint8_t)((bits >> 16) & 0xFFu);
    little[3] = (uint8_t)((bits >> 24) & 0xFFu);
    return put_raw_bytes(writer, little, sizeof little) ? AA_PROTO_OK : AA_PROTO_ERR_OVERFLOW;
}

aa_proto_status aa_proto_put_len_field(aa_proto_writer *writer, uint32_t field,
                                       const uint8_t *data, size_t len)
{
    if (writer == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    if (data == NULL && len != 0u)
        return AA_PROTO_ERR_ARGUMENT;
    if (writer->failed)
        return AA_PROTO_ERR_OVERFLOW;
    aa_proto_status status = put_key(writer, field, AA_PROTO_WIRE_LEN);
    if (status != AA_PROTO_OK)
        return status;
    if (!put_raw_varint(writer, (uint64_t)len))
        return AA_PROTO_ERR_OVERFLOW;
    return put_raw_bytes(writer, data, len) ? AA_PROTO_OK : AA_PROTO_ERR_OVERFLOW;
}

aa_proto_status aa_proto_put_string_field(aa_proto_writer *writer, uint32_t field,
                                          const char *text)
{
    if (text == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    const size_t len = strlen(text);
    if (!aa_proto_utf8_valid(text, len))
        return AA_PROTO_ERR_UTF8;
    return aa_proto_put_len_field(writer, field, (const uint8_t *)text, len);
}

aa_proto_status aa_proto_begin_message(aa_proto_writer *writer, uint32_t field, size_t *marker)
{
    if (writer == NULL || marker == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    if (writer->failed)
        return AA_PROTO_ERR_OVERFLOW;
    const aa_proto_status status = put_key(writer, field, AA_PROTO_WIRE_LEN);
    if (status != AA_PROTO_OK)
        return status;
    *marker = writer->len;
    return AA_PROTO_OK;
}

aa_proto_status aa_proto_end_message(aa_proto_writer *writer, size_t marker)
{
    if (writer == NULL)
        return AA_PROTO_ERR_ARGUMENT;
    if (writer->failed)
        return AA_PROTO_ERR_OVERFLOW;
    if (marker > writer->len)
        return AA_PROTO_ERR_ARGUMENT;

    const size_t body_len = writer->len - marker;
    uint8_t encoded[AA_PROTO_MAX_VARINT_BYTES];
    size_t used = 0u;
    uint64_t remaining = (uint64_t)body_len;
    do {
        uint8_t byte = (uint8_t)(remaining & 0x7Fu);
        remaining >>= 7;
        if (remaining != 0u)
            byte |= 0x80u;
        encoded[used++] = byte;
    } while (remaining != 0u);

    if (writer->cap - writer->len < used) {
        writer->failed = true;
        return AA_PROTO_ERR_OVERFLOW;
    }
    memmove(writer->buf + marker + used, writer->buf + marker, body_len);
    memcpy(writer->buf + marker, encoded, used);
    writer->len += used;
    return AA_PROTO_OK;
}
