/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Wireless bootstrap codec implementation.  See aa_btboot.h for the pinned
 * packet table, the two named guesses and the local policy.
 */
#include "aa_btboot.h"

#include <string.h>

#include "aa_proto.h"

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static bool ip_text_usable(const char *ip)
{
    if (ip == NULL)
        return false;
    const size_t len = strlen(ip);
    if (len == 0u || len > AA_BTBOOT_MAX_IP)
        return false;
    size_t dots = 0u;
    for (size_t i = 0u; i < len; i++) {
        const char c = ip[i];
        if (c == '.') {
            dots++;
            if (i == 0u || i + 1u == len || ip[i - 1u] == '.')
                return false;
            continue;
        }
        if (c < '0' || c > '9')
            return false;
    }
    return dots == 3u;
}

bool aa_btboot_bssid_text(const uint8_t mac[6], char out[AA_BTBOOT_BSSID_LEN + 1u])
{
    static const char hex[] = "0123456789ABCDEF";
    if (mac == NULL || out == NULL)
        return false;
    size_t at = 0u;
    for (size_t i = 0u; i < 6u; i++) {
        out[at++] = hex[(mac[i] >> 4) & 0x0Fu];
        out[at++] = hex[mac[i] & 0x0Fu];
        if (i != 5u)
            out[at++] = ':';
    }
    out[at] = '\0';
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

aa_btboot_status aa_btboot_bssid_parse(const char *text, uint8_t mac[6])
{
    if (text == NULL || mac == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;
    if (strlen(text) != AA_BTBOOT_BSSID_LEN)
        return AA_BTBOOT_ERR_FIELD;
    for (size_t i = 0u; i < 6u; i++) {
        const int hi = hex_value(text[i * 3u]);
        const int lo = hex_value(text[i * 3u + 1u]);
        if (hi < 0 || lo < 0)
            return AA_BTBOOT_ERR_FIELD;
        if (i != 5u && text[i * 3u + 2u] != ':')
            return AA_BTBOOT_ERR_FIELD;
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    return AA_BTBOOT_OK;
}

static aa_btboot_status validate_info(const aa_btboot_info *info)
{
    if (info == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;
    const size_t ssid_len = strlen(info->ssid);
    if (ssid_len == 0u || ssid_len > AA_BTBOOT_MAX_SSID)
        return AA_BTBOOT_ERR_FIELD;
    const size_t key_len = strlen(info->key);
    if (key_len < AA_BTBOOT_MIN_KEY || key_len > AA_BTBOOT_MAX_KEY)
        return AA_BTBOOT_ERR_FIELD;
    uint8_t mac[6];
    if (aa_btboot_bssid_parse(info->bssid, mac) != AA_BTBOOT_OK)
        return AA_BTBOOT_ERR_FIELD;
    if (info->security_mode != AA_BTBOOT_SECURITY_WPA2_PERSONAL)
        return AA_BTBOOT_ERR_FIELD;
    if (info->ap_type != AA_BTBOOT_AP_STATIC && info->ap_type != AA_BTBOOT_AP_DYNAMIC)
        return AA_BTBOOT_ERR_FIELD;
    return AA_BTBOOT_OK;
}

aa_btboot_status aa_btboot_pack(uint16_t message_id, const uint8_t *body, size_t body_len,
                                bool length_includes_message_id, uint8_t *out, size_t out_cap,
                                size_t *out_len)
{
    if (out == NULL || out_len == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;
    if (body == NULL && body_len != 0u)
        return AA_BTBOOT_ERR_ARGUMENT;
    if (body_len > AA_BTBOOT_MAX_BODY)
        return AA_BTBOOT_ERR_LENGTH;
    if (out_cap < AA_BTBOOT_HEADER_LEN + body_len)
        return AA_BTBOOT_ERR_BUFFER;

    write_be16(out, (uint16_t)(length_includes_message_id ? body_len + 2u : body_len));
    write_be16(out + 2u, message_id);
    if (body_len != 0u)
        memcpy(out + AA_BTBOOT_HEADER_LEN, body, body_len);
    *out_len = AA_BTBOOT_HEADER_LEN + body_len;
    return AA_BTBOOT_OK;
}

aa_btboot_status aa_btboot_unpack(const uint8_t *buf, size_t len,
                                  bool length_includes_message_id, uint16_t *message_id,
                                  const uint8_t **body, size_t *body_len, size_t *consumed)
{
    if (buf == NULL || message_id == NULL || body == NULL || body_len == NULL || consumed == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;
    if (len < AA_BTBOOT_HEADER_LEN)
        return AA_BTBOOT_ERR_LENGTH;

    const uint16_t declared = read_be16(buf);
    const size_t minimum = length_includes_message_id ? 2u : 0u;
    if ((size_t)declared < minimum)
        return AA_BTBOOT_ERR_LENGTH;

    const size_t need = length_includes_message_id ? (size_t)declared + 2u
                                                  : (size_t)declared + AA_BTBOOT_HEADER_LEN;
    if (len < need)
        return AA_BTBOOT_ERR_LENGTH;

    *message_id = read_be16(buf + 2u);
    *body = buf + AA_BTBOOT_HEADER_LEN;
    *body_len = length_includes_message_id ? (size_t)declared - 2u : (size_t)declared;
    *consumed = need;
    return AA_BTBOOT_OK;
}

void aa_btboot_set_framing(aa_btboot *boot, bool length_includes_message_id)
{
    if (boot == NULL)
        return;
    boot->length_includes_message_id = length_includes_message_id;
}

static aa_btboot_status encode_start_body(const aa_btboot_start *start, uint8_t *body,
                                          size_t body_cap, size_t *body_len)
{
    if (start == NULL || body == NULL || body_len == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;
    if (!ip_text_usable(start->ip))
        return AA_BTBOOT_ERR_FIELD;
    if (start->port == 0u || start->port > 65535u)
        return AA_BTBOOT_ERR_FIELD;

    aa_proto_writer writer;
    aa_proto_writer_init(&writer, body, body_cap);
    if (aa_proto_put_string_field(&writer, 1u, start->ip) != AA_PROTO_OK)
        return AA_BTBOOT_ERR_BUFFER;
    if (aa_proto_put_varint_field(&writer, 2u, start->port) != AA_PROTO_OK)
        return AA_BTBOOT_ERR_BUFFER;
    *body_len = aa_proto_writer_len(&writer);
    return AA_BTBOOT_OK;
}

static aa_btboot_status encode_info_body(const aa_btboot_info *info, uint8_t *body,
                                         size_t body_cap, size_t *body_len)
{
    const aa_btboot_status valid = validate_info(info);
    if (valid != AA_BTBOOT_OK)
        return valid;
    if (body == NULL || body_len == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;

    aa_proto_writer writer;
    aa_proto_writer_init(&writer, body, body_cap);
    if (aa_proto_put_string_field(&writer, 1u, info->ssid) != AA_PROTO_OK)
        return AA_BTBOOT_ERR_BUFFER;
    if (aa_proto_put_string_field(&writer, 2u, info->key) != AA_PROTO_OK)
        return AA_BTBOOT_ERR_BUFFER;
    if (aa_proto_put_string_field(&writer, 3u, info->bssid) != AA_PROTO_OK)
        return AA_BTBOOT_ERR_BUFFER;
    if (aa_proto_put_varint_field(&writer, 4u, info->security_mode) != AA_PROTO_OK)
        return AA_BTBOOT_ERR_BUFFER;
    if (aa_proto_put_varint_field(&writer, 5u, info->ap_type) != AA_PROTO_OK)
        return AA_BTBOOT_ERR_BUFFER;
    *body_len = aa_proto_writer_len(&writer);
    return AA_BTBOOT_OK;
}

static aa_btboot_status decode_status(const uint8_t *body, size_t body_len, uint32_t *status)
{
    if (body == NULL && body_len != 0u)
        return AA_BTBOOT_ERR_ARGUMENT;
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
            return AA_BTBOOT_ERR_FIELD;
        if (field == 1u && wire == AA_PROTO_WIRE_VARINT) {
            uint64_t value = 0u;
            if (aa_proto_read_varint(&iter, &value) != AA_PROTO_OK)
                return AA_BTBOOT_ERR_FIELD;
            if (value > 0xFFFFFFFFu)
                return AA_BTBOOT_ERR_FIELD;
            *status = (uint32_t)value;
            have_status = true;
            continue;
        }
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            return AA_BTBOOT_ERR_FIELD;
    }
    return have_status ? AA_BTBOOT_OK : AA_BTBOOT_ERR_FIELD;
}

void aa_btboot_reset(aa_btboot *boot)
{
    if (boot == NULL)
        return;
    memset(boot, 0, sizeof *boot);
    boot->state = AA_BTBOOT_IDLE;
    boot->length_includes_message_id = true;
}

aa_btboot_status aa_btboot_set_credentials(aa_btboot *boot, const aa_btboot_info *info)
{
    if (boot == NULL || info == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;
    const aa_btboot_status valid = validate_info(info);
    if (valid != AA_BTBOOT_OK) {
        boot->errors++;
        return valid;
    }
    boot->info = *info;
    boot->credentials_set = true;
    return AA_BTBOOT_OK;
}

aa_btboot_status aa_btboot_begin(aa_btboot *boot, const aa_btboot_start *start, uint8_t *packet,
                                 size_t packet_cap, size_t *packet_len)
{
    if (boot == NULL || start == NULL || packet == NULL || packet_len == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;
    *packet_len = 0u;
    if (boot->state != AA_BTBOOT_IDLE)
        return AA_BTBOOT_ERR_STATE;
    if (!boot->credentials_set) {
        boot->errors++;
        return AA_BTBOOT_ERR_STATE;
    }

    uint8_t body[AA_BTBOOT_MAX_BODY];
    size_t body_len = 0u;
    const aa_btboot_status encoded = encode_start_body(start, body, sizeof body, &body_len);
    if (encoded != AA_BTBOOT_OK) {
        boot->errors++;
        return encoded;
    }
    const aa_btboot_status packed =
        aa_btboot_pack(AA_BTBOOT_MSG_START_REQUEST, body, body_len,
                       boot->length_includes_message_id, packet, packet_cap, packet_len);
    if (packed != AA_BTBOOT_OK) {
        boot->errors++;
        return packed;
    }
    boot->start = *start;
    boot->state = AA_BTBOOT_START_SENT;
    return AA_BTBOOT_OK;
}

aa_btboot_status aa_btboot_on_packet(aa_btboot *boot, uint16_t message_id, const uint8_t *body,
                                     size_t body_len, uint8_t *reply, size_t reply_cap,
                                     size_t *reply_len)
{
    if (boot == NULL || reply == NULL || reply_len == NULL)
        return AA_BTBOOT_ERR_ARGUMENT;
    *reply_len = 0u;

    switch (message_id) {
    case AA_BTBOOT_MSG_START_RESPONSE: {
        if (boot->state != AA_BTBOOT_START_SENT)
            return AA_BTBOOT_ERR_STATE;
        uint32_t status = 0u;
        if (decode_status(body, body_len, &status) != AA_BTBOOT_OK) {
            boot->errors++;
            return AA_BTBOOT_ERR_FIELD;
        }
        if (status != 0u) {
            boot->state = AA_BTBOOT_FAILED;
            boot->errors++;
            return AA_BTBOOT_ERR_REFUSED;
        }
        boot->state = AA_BTBOOT_AWAIT_INFO;
        return AA_BTBOOT_OK;
    }

    case AA_BTBOOT_MSG_INFO_REQUEST: {
        if (boot->state != AA_BTBOOT_AWAIT_INFO)
            return AA_BTBOOT_ERR_STATE;
        uint8_t body_out[AA_BTBOOT_MAX_BODY];
        size_t body_out_len = 0u;
        const aa_btboot_status encoded =
            encode_info_body(&boot->info, body_out, sizeof body_out, &body_out_len);
        if (encoded != AA_BTBOOT_OK) {
            boot->state = AA_BTBOOT_FAILED;
            boot->errors++;
            return encoded;
        }
        const aa_btboot_status packed =
            aa_btboot_pack(AA_BTBOOT_MSG_INFO_RESPONSE, body_out, body_out_len,
                           boot->length_includes_message_id, reply, reply_cap, reply_len);
        if (packed != AA_BTBOOT_OK) {
            boot->errors++;
            return packed;
        }
        boot->state = AA_BTBOOT_INFO_SENT;
        return AA_BTBOOT_OK;
    }

    case AA_BTBOOT_MSG_CONNECT_STATUS: {
        if (boot->state != AA_BTBOOT_INFO_SENT)
            return AA_BTBOOT_ERR_STATE;
        uint32_t status = 0u;
        if (decode_status(body, body_len, &status) != AA_BTBOOT_OK) {
            boot->errors++;
            return AA_BTBOOT_ERR_FIELD;
        }
        if (status != 0u) {
            boot->state = AA_BTBOOT_FAILED;
            boot->errors++;
            return AA_BTBOOT_ERR_REFUSED;
        }
        boot->state = AA_BTBOOT_CONNECTED;
        return AA_BTBOOT_OK;
    }

    default:
        boot->ignored++;
        return AA_BTBOOT_OK;
    }
}

const char *aa_btboot_state_name(aa_btboot_state state)
{
    switch (state) {
    case AA_BTBOOT_IDLE:
        return "idle";
    case AA_BTBOOT_START_SENT:
        return "start_sent";
    case AA_BTBOOT_AWAIT_INFO:
        return "await_info";
    case AA_BTBOOT_INFO_SENT:
        return "info_sent";
    case AA_BTBOOT_CONNECTED:
        return "connected";
    case AA_BTBOOT_FAILED:
        return "failed";
    }
    return "unknown";
}

const char *aa_btboot_status_name(aa_btboot_status status)
{
    switch (status) {
    case AA_BTBOOT_OK:
        return "ok";
    case AA_BTBOOT_ERR_ARGUMENT:
        return "argument";
    case AA_BTBOOT_ERR_STATE:
        return "state";
    case AA_BTBOOT_ERR_LENGTH:
        return "length";
    case AA_BTBOOT_ERR_BUFFER:
        return "buffer";
    case AA_BTBOOT_ERR_FIELD:
        return "field";
    case AA_BTBOOT_ERR_REFUSED:
        return "refused";
    }
    return "unknown";
}
