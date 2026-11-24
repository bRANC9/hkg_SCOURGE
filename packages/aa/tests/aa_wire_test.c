/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the Android Auto frame-layer codec.  Fixed external
 * byte vectors first, then round-trips, a streaming feed and a malformed
 * corpus; no wire peer and no transport is involved.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_channels.h"
#include "../src/aa_wire.h"

static int g_failures;

static void check(const char *name, bool ok)
{
    if (!ok) {
        g_failures++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t cap)
{
    const size_t hex_len = strlen(hex);
    if ((hex_len % 2u) != 0u || hex_len / 2u > cap)
        return 0u;
    size_t count = 0u;
    for (size_t i = 0u; i < hex_len; i += 2u) {
        int hi = -1;
        int lo = -1;
        const char a = hex[i];
        const char b = hex[i + 1u];
        if (a >= '0' && a <= '9')
            hi = a - '0';
        else if (a >= 'a' && a <= 'f')
            hi = a - 'a' + 10;
        if (b >= '0' && b <= '9')
            lo = b - '0';
        else if (b >= 'a' && b <= 'f')
            lo = b - 'a' + 10;
        if (hi < 0 || lo < 0)
            return 0u;
        out[count++] = (uint8_t)((hi << 4) | lo);
    }
    return count;
}

/* CONTROL / BULK / plain version request: id 0x0001, major 1, minor 0. */
static const char *const VECTOR_VERSION_REQUEST = "00030006000100010000";
/* CONTROL / BULK / encrypted service discovery response: id 0x0006 plus a
 * three-field body. */
static const char *const VECTOR_DISCOVERY_ENCRYPTED = "000b0009000608011203616263";
/* VIDEO FIRST frame: extended size, 4 payload bytes, total message 6. */
static const char *const VECTOR_FIRST_VIDEO = "030100040000000641414141";

static void test_parse_fixed_frames(void)
{
    uint8_t buf[64];
    aa_wire_frame frame;

    size_t len = hex_to_bytes("00030006000100010000", buf, sizeof buf);
    check("vector1 len", len == 10u);
    check("vector1 parse", aa_wire_parse_frame(buf, len, &frame) == AA_WIRE_OK);
    check("vector1 channel", frame.channel == AA_CHANNEL_CONTROL);
    check("vector1 bulk", frame.first == false && frame.last == true);
    check("vector1 plain", frame.control == false && frame.encrypted == false);
    check("vector1 size", frame.payload_len == 6u && frame.frame_len == 10u);
    check("vector1 id", frame.payload != NULL && frame.payload[0] == 0x00 && frame.payload[1] == 0x01);

    len = hex_to_bytes("000b0009000608011203616263", buf, sizeof buf);
    check("vector2 len", len == 13u);
    check("vector2 parse", aa_wire_parse_frame(buf, len, &frame) == AA_WIRE_OK);
    check("vector2 encrypted", frame.encrypted == true);
    check("vector2 size", frame.payload_len == 9u);

    len = hex_to_bytes("030100040000000641414141", buf, sizeof buf);
    check("vector3 len", len == 12u);
    check("vector3 parse", aa_wire_parse_frame(buf, len, &frame) == AA_WIRE_OK);
    check("vector3 first", frame.first == true && frame.last == false);
    check("vector3 channel", frame.channel == AA_CHANNEL_VIDEO);
    check("vector3 total", frame.total_len == 6u);
    check("vector3 frame len", frame.frame_len == 12u);
    check("vector3 payload", frame.payload_len == 4u && frame.payload[0] == 0x41);

    len = hex_to_bytes("030200024242", buf, sizeof buf);
    check("vector4 len", len == 6u);
    check("vector4 parse", aa_wire_parse_frame(buf, len, &frame) == AA_WIRE_OK);
    check("vector4 last", frame.last == true && frame.first == false);

    /* The encoder must reproduce the plain vector byte for byte. */
    const uint8_t payload[6] = {0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    uint8_t out[32];
    size_t out_len = 0u;
    check("encode version request",
          aa_wire_encode_message(AA_CHANNEL_CONTROL, false, false, payload, sizeof payload,
                                 AA_WIRE_MAX_FRAME_PAYLOAD, out, sizeof out, &out_len) == AA_WIRE_OK);
    check("encode version request len", out_len == 10u);
    uint8_t expected[16];
    const size_t expected_len = hex_to_bytes(VECTOR_VERSION_REQUEST, expected, sizeof expected);
    check("encode version request bytes",
          expected_len == out_len && memcmp(expected, out, out_len) == 0);

    const uint8_t discovery[9] = {0x00, 0x06, 0x08, 0x01, 0x12, 0x03, 0x61, 0x62, 0x63};
    check("encode encrypted discovery",
          aa_wire_encode_message(AA_CHANNEL_CONTROL, false, true, discovery, sizeof discovery,
                                 AA_WIRE_MAX_FRAME_PAYLOAD, out, sizeof out, &out_len) == AA_WIRE_OK);
    const size_t discovery_len =
        hex_to_bytes(VECTOR_DISCOVERY_ENCRYPTED, expected, sizeof expected);
    check("encode encrypted discovery bytes",
          discovery_len == out_len && memcmp(expected, out, out_len) == 0);
}

static void test_decode_fixed_messages(void)
{
    uint8_t buf[64];
    uint8_t msg[64];
    aa_wire_message message;
    aa_wire_decode state;
    size_t consumed = 0u;

    size_t len = hex_to_bytes(VECTOR_VERSION_REQUEST, buf, sizeof buf);
    aa_wire_decode_init(&state);
    check("decode version request",
          aa_wire_decode_message(&state, buf, len, msg, sizeof msg, &message, &consumed) == AA_WIRE_OK);
    check("decode version request consumed", consumed == len);
    check("decode version request payload",
          message.payload_len == 6u && message.channel == AA_CHANNEL_CONTROL &&
              message.payload == msg && memcmp(msg, buf + 4, 6) == 0);

    /* A two-frame message assembled from the fixed FIRST and LAST vectors. */
    uint8_t first[16];
    uint8_t last[16];
    const size_t first_len = hex_to_bytes("030100040000000641414141", first, sizeof first);
    const size_t last_len = hex_to_bytes("030200024242", last, sizeof last);
    uint8_t stream[32];
    memcpy(stream, first, first_len);
    memcpy(stream + first_len, last, last_len);
    aa_wire_decode_init(&state);
    check("decode two frames",
          aa_wire_decode_message(&state, stream, first_len + last_len, msg, sizeof msg, &message,
                                 &consumed) == AA_WIRE_OK);
    check("decode two frames consumed", consumed == first_len + last_len);
    check("decode two frames payload",
          message.payload_len == 6u && memcmp(msg, "AAAABB", 6) == 0);
    check("decode two frames channel", message.channel == AA_CHANNEL_VIDEO);

    /* Reference-sender quirk: an empty trailing LAST frame is accepted. */
    const size_t exact_len = hex_to_bytes("000100040000000441414141", first, sizeof first);
    const size_t empty_len = hex_to_bytes("00020000", last, sizeof last);
    memcpy(stream, first, exact_len);
    memcpy(stream + exact_len, last, empty_len);
    aa_wire_decode_init(&state);
    check("decode empty last",
          aa_wire_decode_message(&state, stream, exact_len + empty_len, msg, sizeof msg, &message,
                                 &consumed) == AA_WIRE_OK);
    check("decode empty last payload", message.payload_len == 4u && memcmp(msg, "AAAA", 4) == 0);
    check("decode empty last consumed", consumed == exact_len + empty_len);
}

static void test_fragmentation_roundtrip(void)
{
    uint8_t payload[5000];
    for (size_t i = 0u; i < sizeof payload; i++)
        payload[i] = (uint8_t)((i * 31u + 7u) & 0xFFu);

    uint8_t stream[5200];
    size_t stream_len = 0u;
    check("fragment encode",
          aa_wire_encode_message(AA_CHANNEL_VIDEO, false, false, payload, sizeof payload, 4096u,
                                 stream, sizeof stream, &stream_len) == AA_WIRE_OK);
    /* FIRST: 2 header + 6 size + 4096; LAST: 2 + 2 + 904. */
    check("fragment stream length", stream_len == 4104u + 908u);
    check("fragment first header", stream[0] == AA_CHANNEL_VIDEO && stream[1] == AA_WIRE_FLAG_FIRST);
    check("fragment total field",
          stream[4] == 0x00u && stream[5] == 0x00u && stream[6] == 0x13u && stream[7] == 0x88u);
    check("fragment last header",
          stream[4104] == AA_CHANNEL_VIDEO && stream[4105] == AA_WIRE_FLAG_LAST);

    uint8_t msg[5200];
    aa_wire_message message;
    aa_wire_decode state;
    aa_wire_decode_init(&state);
    size_t consumed = 0u;
    check("fragment decode",
          aa_wire_decode_message(&state, stream, stream_len, msg, sizeof msg, &message, &consumed) ==
              AA_WIRE_OK);
    check("fragment decode consumed", consumed == stream_len);
    check("fragment decode length", message.payload_len == sizeof payload);
    check("fragment decode bytes", memcmp(msg, payload, sizeof payload) == 0);
}

static void test_streaming_feed(void)
{
    uint8_t payload[300];
    for (size_t i = 0u; i < sizeof payload; i++)
        payload[i] = (uint8_t)(i ^ 0x5Au);

    uint8_t stream[512];
    size_t stream_len = 0u;
    check("stream encode",
          aa_wire_encode_message(AA_CHANNEL_MEDIA_AUDIO, false, true, payload, sizeof payload, 64u,
                                 stream, sizeof stream, &stream_len) == AA_WIRE_OK);

    aa_wire_decode state;
    aa_wire_decode_init(&state);
    uint8_t pending[512];
    size_t pending_len = 0u;
    size_t fed = 0u;
    uint8_t msg[512];
    bool complete = false;

    for (int guard = 0; guard < 4096 && !complete; guard++) {
        if (fed < stream_len && pending_len + 1u < sizeof pending) {
            pending[pending_len++] = stream[fed++];
        } else if (fed >= stream_len && pending_len == 0u) {
            break;
        }
        aa_wire_message message;
        size_t consumed = 0u;
        const aa_wire_status status =
            aa_wire_decode_message(&state, pending, pending_len, msg, sizeof msg, &message, &consumed);
        if (consumed != 0u) {
            memmove(pending, pending + consumed, pending_len - consumed);
            pending_len -= consumed;
        }
        if (status == AA_WIRE_OK) {
            check("stream length", message.payload_len == sizeof payload);
            check("stream bytes", memcmp(msg, payload, sizeof payload) == 0);
            check("stream channel", message.channel == AA_CHANNEL_MEDIA_AUDIO);
            check("stream encrypted", message.encrypted == true);
            complete = true;
        } else if (status != AA_WIRE_END && status != AA_WIRE_ERR_TRUNCATED) {
            check("stream unexpected status", false);
            break;
        }
    }
    check("stream complete", complete);
}

static void test_partial_consumption(void)
{
    uint8_t buf[64];
    uint8_t msg[64];
    aa_wire_message message;
    aa_wire_decode state;
    size_t consumed = 0u;

    /* A lone FIRST frame is a complete frame but an incomplete message: the
     * caller must be told how many bytes it may drop, otherwise the next call
     * re-parses the first frame and reports a protocol error. */
    const size_t first_len = hex_to_bytes(VECTOR_FIRST_VIDEO, buf, sizeof buf);
    aa_wire_decode_init(&state);
    consumed = 99u;
    check("partial returns end",
          aa_wire_decode_message(&state, buf, first_len, msg, sizeof msg, &message, &consumed) ==
              AA_WIRE_END);
    check("partial consumes the frame", consumed == first_len);
    check("partial state active", state.active && state.received == 4u && state.total == 6u);

    /* A frame that is not complete yet consumes nothing at all. */
    aa_wire_decode_init(&state);
    consumed = 99u;
    check("truncated returns truncated",
          aa_wire_decode_message(&state, buf, first_len - 1u, msg, sizeof msg, &message, &consumed) ==
              AA_WIRE_ERR_TRUNCATED);
    check("truncated consumes nothing", consumed == 0u);
}

static void test_malformed_corpus(void)
{
    uint8_t buf[64] = {0};
    uint8_t msg[64];
    aa_wire_frame frame;
    aa_wire_message message;
    aa_wire_decode state;
    size_t consumed = 0u;

    check("empty parse", aa_wire_parse_frame(buf, 0u, &frame) == AA_WIRE_ERR_TRUNCATED);
    buf[0] = AA_CHANNEL_CONTROL;
    buf[1] = AA_WIRE_FLAG_BULK;
    check("header only", aa_wire_parse_frame(buf, 2u, &frame) == AA_WIRE_ERR_TRUNCATED);

    size_t len = hex_to_bytes("00130006000100010000", buf, sizeof buf);
    check("unknown flag bits", aa_wire_parse_frame(buf, len, &frame) == AA_WIRE_ERR_FLAGS);
    len = hex_to_bytes("00034001", buf, sizeof buf);
    check("oversize frame payload", aa_wire_parse_frame(buf, len, &frame) == AA_WIRE_ERR_OVERSIZE);

    len = hex_to_bytes("0001000000000000", buf, sizeof buf);
    check("first with zero total", aa_wire_parse_frame(buf, len, &frame) == AA_WIRE_ERR_LENGTH);
    len = hex_to_bytes("000100040000000241414141", buf, sizeof buf);
    check("first below frame size", aa_wire_parse_frame(buf, len, &frame) == AA_WIRE_ERR_LENGTH);

    /* MIDDLE frame without an active message. */
    len = hex_to_bytes("030000024141", buf, sizeof buf);
    aa_wire_decode_init(&state);
    check("middle without start",
          aa_wire_decode_message(&state, buf, len, msg, sizeof msg, &message, &consumed) ==
              AA_WIRE_ERR_FLAGS);

    /* Interleaved channel: FIRST on VIDEO, then a CONTROL MIDDLE. */
    uint8_t stream[64];
    uint8_t part[32];
    size_t first_len = hex_to_bytes(VECTOR_FIRST_VIDEO, part, sizeof part);
    memcpy(stream, part, first_len);
    size_t middle_len = hex_to_bytes("000000024141", part, sizeof part);
    memcpy(stream + first_len, part, middle_len);
    aa_wire_decode_init(&state);
    check("interleaved channel",
          aa_wire_decode_message(&state, stream, first_len + middle_len, msg, sizeof msg, &message,
                                 &consumed) == AA_WIRE_ERR_INTERLEAVED);

    /* Encrypted bit changes mid-message. */
    first_len = hex_to_bytes(VECTOR_FIRST_VIDEO, part, sizeof part);
    memcpy(stream, part, first_len);
    middle_len = hex_to_bytes("030a00024141", part, sizeof part);
    memcpy(stream + first_len, part, middle_len);
    aa_wire_decode_init(&state);
    check("encrypted bit change",
          aa_wire_decode_message(&state, stream, first_len + middle_len, msg, sizeof msg, &message,
                                 &consumed) == AA_WIRE_ERR_INTERLEAVED);

    /* A nested FIRST inside an active message. */
    first_len = hex_to_bytes(VECTOR_FIRST_VIDEO, part, sizeof part);
    memcpy(stream, part, first_len);
    memcpy(stream + first_len, part, first_len);
    aa_wire_decode_init(&state);
    check("nested first",
          aa_wire_decode_message(&state, stream, first_len * 2u, msg, sizeof msg, &message,
                                 &consumed) == AA_WIRE_ERR_FLAGS);

    /* LAST before the declared total. */
    first_len = hex_to_bytes(VECTOR_FIRST_VIDEO, part, sizeof part);
    memcpy(stream, part, first_len);
    middle_len = hex_to_bytes("0302000141", part, sizeof part);
    memcpy(stream + first_len, part, middle_len);
    aa_wire_decode_init(&state);
    check("short message",
          aa_wire_decode_message(&state, stream, first_len + middle_len, msg, sizeof msg, &message,
                                 &consumed) == AA_WIRE_ERR_LENGTH);

    /* Payload beyond the declared total. */
    first_len = hex_to_bytes("030100040000000641414141", part, sizeof part);
    memcpy(stream, part, first_len);
    middle_len = hex_to_bytes("03020003414243", part, sizeof part);
    memcpy(stream + first_len, part, middle_len);
    aa_wire_decode_init(&state);
    check("overlong message",
          aa_wire_decode_message(&state, stream, first_len + middle_len, msg, sizeof msg, &message,
                                 &consumed) == AA_WIRE_ERR_LENGTH);

    /* A message that does not fit the caller's buffer. */
    len = hex_to_bytes(VECTOR_DISCOVERY_ENCRYPTED, buf, sizeof buf);
    aa_wire_decode_init(&state);
    check("oversize bulk",
          aa_wire_decode_message(&state, buf, len, msg, 4u, &message, &consumed) ==
              AA_WIRE_ERR_OVERSIZE);
    len = hex_to_bytes("0301000400000bb841414141", buf, sizeof buf);
    aa_wire_decode_init(&state);
    check("oversize first total",
          aa_wire_decode_message(&state, buf, len, msg, 64u, &message, &consumed) ==
              AA_WIRE_ERR_OVERSIZE);

    /* Encoder argument and bound refusals. */
    uint8_t out[8];
    size_t out_len = 0u;
    const uint8_t eight[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    check("encode total on last",
          aa_wire_encode_frame(AA_CHANNEL_CONTROL, AA_WIRE_FLAG_LAST, 4u, NULL, 0u, out, sizeof out,
                               &out_len) == AA_WIRE_ERR_LENGTH);
    check("encode first without total",
          aa_wire_encode_frame(AA_CHANNEL_CONTROL, AA_WIRE_FLAG_FIRST, 0u, NULL, 0u, out, sizeof out,
                               &out_len) == AA_WIRE_ERR_LENGTH);
    check("encode small buffer",
          aa_wire_encode_frame(AA_CHANNEL_CONTROL, AA_WIRE_FLAG_BULK, 0u, eight, sizeof eight, out,
                               sizeof out, &out_len) == AA_WIRE_ERR_OVERSIZE);
    check("encode zero chunk",
          aa_wire_encode_message(AA_CHANNEL_CONTROL, false, false, NULL, 0u, 0u, out, sizeof out,
                                 &out_len) == AA_WIRE_ERR_ARGUMENT);

    /* An error resets the state, so a fresh valid frame decodes again. */
    len = hex_to_bytes(VECTOR_VERSION_REQUEST, buf, sizeof buf);
    check("recovery after error",
          aa_wire_decode_message(&state, buf, len, msg, sizeof msg, &message, &consumed) == AA_WIRE_OK);
}

static void test_random_roundtrip(void)
{
    uint32_t seed = 0x12345678u;
    uint8_t payload[2048];
    uint8_t stream[4096];
    uint8_t msg[4096];

    for (int iteration = 0; iteration < 200; iteration++) {
        seed = seed * 1664525u + 1013904223u;
        const size_t payload_len = (size_t)(seed >> 24) * 8u % (sizeof payload + 1u);
        seed = seed * 1664525u + 1013904223u;
        const size_t chunk = 8u + (size_t)(seed >> 26) * 8u % 56u;
        seed = seed * 1664525u + 1013904223u;
        const uint8_t channel = (uint8_t)(seed >> 27);
        const bool control = (seed & 0x10u) != 0u;
        const bool encrypted = (seed & 0x20u) != 0u;

        for (size_t i = 0u; i < payload_len; i++) {
            seed = seed * 1664525u + 1013904223u;
            payload[i] = (uint8_t)(seed >> 16);
        }

        size_t stream_len = 0u;
        const aa_wire_status enc = aa_wire_encode_message(channel, control, encrypted, payload,
                                                          payload_len, chunk, stream, sizeof stream,
                                                          &stream_len);
        if (enc != AA_WIRE_OK) {
            check("random encode", false);
            return;
        }

        aa_wire_decode state;
        aa_wire_decode_init(&state);
        aa_wire_message message;
        size_t consumed = 0u;
        const aa_wire_status dec = aa_wire_decode_message(&state, stream, stream_len, msg, sizeof msg,
                                                          &message, &consumed);
        if (dec != AA_WIRE_OK || consumed != stream_len || message.payload_len != payload_len ||
            message.channel != channel || message.control != control ||
            message.encrypted != encrypted || memcmp(msg, payload, payload_len) != 0) {
            check("random roundtrip", false);
            return;
        }
    }
    check("random roundtrip", true);
}

static void test_channel_vocabulary(void)
{
    check("channel control", aa_channel_known(AA_CHANNEL_CONTROL));
    check("channel bluetooth", aa_channel_known(AA_CHANNEL_BLUETOOTH));
    check("channel navigation", aa_channel_known(AA_CHANNEL_NAVIGATION));
    check("channel unknown", !aa_channel_known(10u) && !aa_channel_known(AA_CHANNEL_UNKNOWN));
    check("channel name control", strcmp(aa_channel_name(AA_CHANNEL_CONTROL), "control") == 0);
    check("channel name unknown", strcmp(aa_channel_name(42u), "unknown") == 0);
    check("feature name ev", strcmp(aa_feature_name(AA_FEATURE_EV_ENERGY), "ev_energy") == 0);
    check("feature name navigation",
          strcmp(aa_feature_name(AA_FEATURE_NAVIGATION), "navigation") == 0);
    check("status name ok", strcmp(aa_wire_status_name(AA_WIRE_OK), "ok") == 0);
    check("status name interleaved",
          strcmp(aa_wire_status_name(AA_WIRE_ERR_INTERLEAVED), "interleaved") == 0);
}

int main(void)
{
    test_parse_fixed_frames();
    test_decode_fixed_messages();
    test_fragmentation_roundtrip();
    test_streaming_feed();
    test_partial_consumption();
    test_malformed_corpus();
    test_random_roundtrip();
    test_channel_vocabulary();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_wire: all checks passed\n");
    return 0;
}
