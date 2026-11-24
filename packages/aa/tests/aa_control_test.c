/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the control-channel state machine: fixed message
 * vectors, the happy path to READY, channel-open accept/refuse, ping echo,
 * shutdown, and the error policy for state, channel and parse failures.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_channels.h"
#include "../src/aa_control.h"
#include "../src/aa_proto.h"

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

static bool payload_is(const aa_control_out *out, const char *hex)
{
    uint8_t expected[64];
    const size_t expected_len = hex_to_bytes(hex, expected, sizeof expected);
    return expected_len == out->payload_len && memcmp(out->payload, expected, expected_len) == 0;
}

static void drive_to_auth_sent(aa_control *control, aa_control_out *out)
{
    aa_control_reset(control, 1u, 0u);
    (void)aa_control_start(control, out);
    uint8_t body[8];
    size_t body_len = hex_to_bytes("000100000000", body, sizeof body);
    (void)aa_control_on_message(control, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_RESPONSE, body,
                                body_len, out);
    (void)aa_control_on_tls_established(control, out);
}

static void test_start_and_version(void)
{
    aa_control control;
    aa_control_out out;

    aa_control_reset(&control, 1u, 0u);
    check("start ok", aa_control_start(&control, &out) == AA_CONTROL_OK);
    check("start payload", payload_is(&out, "000100010000"));
    check("start state", control.state == AA_CONTROL_VERSION_SENT && out.state == control.state);

    check("start twice", aa_control_start(&control, &out) == AA_CONTROL_ERR_STATE);
    check("start twice no reply", out.payload_len == 0u);

    aa_control_reset(&control, 3u, 2u);
    check("start versions", aa_control_start(&control, &out) == AA_CONTROL_OK);
    check("start versions payload", payload_is(&out, "000100030002"));

    /* A matching version response hands the session to the TLS step. */
    aa_control_reset(&control, 1u, 0u);
    (void)aa_control_start(&control, &out);
    uint8_t body[8];
    size_t body_len = hex_to_bytes("000100000000", body, sizeof body);
    check("version match",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_RESPONSE, body,
                                body_len, &out) == AA_CONTROL_OK);
    check("version match state", control.state == AA_CONTROL_TLS_PENDING);
    check("version match no reply", out.payload_len == 0u && out.event == AA_CONTROL_EVENT_NONE);
    check("version match records peer", control.peer_major == 1u && control.peer_minor == 0u);
    check("version matched flag", control.version_matched);

    check("auth after tls ok", aa_control_on_tls_established(&control, &out) == AA_CONTROL_OK);
    check("auth payload", payload_is(&out, "00040800"));
    check("auth state", control.state == AA_CONTROL_AUTH_SENT);
    check("auth twice", aa_control_on_tls_established(&control, &out) == AA_CONTROL_ERR_STATE);

    /* A mismatched version is terminal. */
    aa_control_reset(&control, 1u, 0u);
    (void)aa_control_start(&control, &out);
    body_len = hex_to_bytes("00010000ffff", body, sizeof body);
    check("version mismatch",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_RESPONSE, body,
                                body_len, &out) == AA_CONTROL_ERR_VERSION);
    check("version mismatch state",
          control.state == AA_CONTROL_FAILED && out.event == AA_CONTROL_EVENT_FAILED);
    check("version mismatch no reply", out.payload_len == 0u);

    /* A short version response is a parse error, not a state error. */
    aa_control_reset(&control, 1u, 0u);
    (void)aa_control_start(&control, &out);
    body_len = hex_to_bytes("000100", body, sizeof body);
    check("version short",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_RESPONSE, body,
                                body_len, &out) == AA_CONTROL_ERR_PARSE);
    check("version short state", control.state == AA_CONTROL_VERSION_SENT);
}

static void test_discovery(void)
{
    aa_control control;
    aa_control_out out;
    drive_to_auth_sent(&control, &out);

    uint8_t body[64];
    const size_t body_len = hex_to_bytes("220550686f6e652a07416e64726f6964", body, sizeof body);
    check("discovery ok",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL,
                                AA_CONTROL_SERVICE_DISCOVERY_REQUEST, body, body_len,
                                &out) == AA_CONTROL_OK);
    check("discovery state", control.state == AA_CONTROL_READY && out.event == AA_CONTROL_EVENT_READY);
    check("discovery records name",
          control.device_name_len == 5u && strcmp(control.device_name, "Phone") == 0);
    check("discovery payload id", out.payload_len > 2u && out.payload[0] == 0x00u &&
                                      out.payload[1] == 0x06u);

    /* Parse the response back: three channel descriptors, then identity. */
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, out.payload + 2u, out.payload_len - 2u);
    uint32_t channel_ids[8];
    size_t channel_count = 0u;
    char head_unit[32];
    size_t head_unit_len = 0u;
    bool left_hand_drive = false;
    bool hide_clock = false;

    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        if (aa_proto_next(&iter, &field, &wire) != AA_PROTO_OK)
            break;
        if (field == 1u && wire == AA_PROTO_WIRE_LEN) {
            const uint8_t *desc = NULL;
            size_t desc_len = 0u;
            if (aa_proto_read_len(&iter, &desc, &desc_len) != AA_PROTO_OK)
                break;
            aa_proto_iter sub;
            aa_proto_iter_init(&sub, desc, desc_len);
            uint32_t sub_field = 0u;
            uint8_t sub_wire = 0u;
            if (aa_proto_next(&sub, &sub_field, &sub_wire) != AA_PROTO_OK || sub_field != 1u ||
                sub_wire != AA_PROTO_WIRE_VARINT)
                break;
            uint64_t value = 0u;
            if (aa_proto_read_varint(&sub, &value) != AA_PROTO_OK)
                break;
            if (channel_count < sizeof channel_ids / sizeof channel_ids[0])
                channel_ids[channel_count++] = (uint32_t)value;
            continue;
        }
        if (field == 2u && wire == AA_PROTO_WIRE_LEN) {
            if (aa_proto_read_string(&iter, head_unit, sizeof head_unit, &head_unit_len) != AA_PROTO_OK)
                break;
            continue;
        }
        if (field == 6u && wire == AA_PROTO_WIRE_VARINT) {
            uint64_t value = 0u;
            if (aa_proto_read_varint(&iter, &value) != AA_PROTO_OK)
                break;
            left_hand_drive = value == 1u;
            continue;
        }
        if (field == 12u && wire == AA_PROTO_WIRE_VARINT) {
            uint64_t value = 0u;
            if (aa_proto_read_varint(&iter, &value) != AA_PROTO_OK)
                break;
            hide_clock = value == 1u;
            continue;
        }
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            break;
    }

    check("discovery channel count", channel_count == 3u);
    check("discovery channel sensor", channel_count == 3u && channel_ids[0] == 2u);
    check("discovery channel navigation", channel_count == 3u && channel_ids[1] == 9u);
    check("discovery channel input", channel_count == 3u && channel_ids[2] == 1u);
    check("discovery head unit name",
          head_unit_len == strlen(AA_CONTROL_HEAD_UNIT_NAME) &&
              strcmp(head_unit, AA_CONTROL_HEAD_UNIT_NAME) == 0);
    check("discovery left hand drive", left_hand_drive);
    check("discovery hide clock", hide_clock);

    /* The same request again is refused: discovery happens once per session. */
    check("discovery twice",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL,
                                AA_CONTROL_SERVICE_DISCOVERY_REQUEST, body, body_len,
                                &out) == AA_CONTROL_ERR_STATE);

    /* A malformed request in the state that expects it is a parse error and
     * leaves the state alone, so the transport can decide whether to drop the
     * session. */
    aa_control fresh;
    drive_to_auth_sent(&fresh, &out);
    const size_t bad_len = hex_to_bytes("22054142", body, sizeof body);
    check("discovery malformed",
          aa_control_on_message(&fresh, AA_CHANNEL_CONTROL,
                                AA_CONTROL_SERVICE_DISCOVERY_REQUEST, body, bad_len,
                                &out) == AA_CONTROL_ERR_PARSE);
    check("discovery malformed state", fresh.state == AA_CONTROL_AUTH_SENT &&
                                            fresh.errors == 1u && out.payload_len == 0u);
}

static void test_channel_open(void)
{
    aa_control control;
    aa_control_out out;
    drive_to_auth_sent(&control, &out);

    uint8_t body[16];
    size_t body_len = hex_to_bytes("08001002", body, sizeof body);
    check("open before discovery",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_REQUEST, body,
                                body_len, &out) == AA_CONTROL_ERR_STATE);

    /* Reach READY via a minimal discovery request. */
    body_len = hex_to_bytes("220550686f6e65", body, sizeof body);
    (void)aa_control_on_message(&control, AA_CHANNEL_CONTROL,
                                AA_CONTROL_SERVICE_DISCOVERY_REQUEST, body, body_len, &out);

    body_len = hex_to_bytes("08001002", body, sizeof body);
    check("open sensor",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_REQUEST, body,
                                body_len, &out) == AA_CONTROL_OK);
    check("open sensor accepted", payload_is(&out, "00080800"));
    check("open sensor mask", (control.opened_mask & (1u << 2)) != 0u);

    body_len = hex_to_bytes("0800104d", body, sizeof body);
    check("open unknown channel",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_REQUEST, body,
                                body_len, &out) == AA_CONTROL_OK);
    check("open unknown refused", payload_is(&out, "00080801"));

    body_len = hex_to_bytes("08001009", body, sizeof body);
    check("open navigation",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_REQUEST, body,
                                body_len, &out) == AA_CONTROL_OK);
    check("open navigation accepted", payload_is(&out, "00080800"));
    check("open navigation mask", (control.opened_mask & (1u << 9)) != 0u);
}

static void test_ping_and_shutdown(void)
{
    aa_control control;
    aa_control_out out;
    drive_to_auth_sent(&control, &out);

    uint8_t body[16];
    size_t body_len = hex_to_bytes("08ac02", body, sizeof body);
    check("ping ok",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_PING_REQUEST, body,
                                body_len, &out) == AA_CONTROL_OK);
    check("ping echo", payload_is(&out, "000c08ac02"));

    const uint32_t ignored_before = control.ignored;
    body_len = hex_to_bytes("08ac02", body, sizeof body);
    check("ping response ignored",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_PING_RESPONSE, body,
                                body_len, &out) == AA_CONTROL_OK);
    check("ping response counted", control.ignored == ignored_before + 1u && out.payload_len == 0u);

    body_len = hex_to_bytes("0801", body, sizeof body);
    check("shutdown ok",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_SHUTDOWN_REQUEST, body,
                                body_len, &out) == AA_CONTROL_OK);
    check("shutdown reply", payload_is(&out, "0010"));
    check("shutdown state",
          control.state == AA_CONTROL_SHUTDOWN && out.event == AA_CONTROL_EVENT_SHUTDOWN);

    /* Shutdown while still negotiating is a state error. */
    aa_control_reset(&control, 1u, 0u);
    (void)aa_control_start(&control, &out);
    body_len = hex_to_bytes("0801", body, sizeof body);
    check("shutdown early",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_SHUTDOWN_REQUEST, body,
                                body_len, &out) == AA_CONTROL_ERR_STATE);
}

static void test_errors_and_vocabulary(void)
{
    aa_control control;
    aa_control_out out;
    drive_to_auth_sent(&control, &out);

    const uint32_t ignored_before = control.ignored;
    check("unknown message tolerated",
          aa_control_on_message(&control, AA_CHANNEL_CONTROL, 0x00AAu, NULL, 0u, &out) ==
              AA_CONTROL_OK);
    check("unknown message counted", control.ignored == ignored_before + 1u);

    const uint32_t errors_before = control.errors;
    check("wrong channel",
          aa_control_on_message(&control, AA_CHANNEL_SENSOR, AA_CONTROL_PING_REQUEST, NULL, 0u,
                                &out) == AA_CONTROL_ERR_CHANNEL);
    check("wrong channel counted", control.errors == errors_before + 1u);

    check("null body with length", aa_control_on_message(&control, AA_CHANNEL_CONTROL,
                                                         AA_CONTROL_PING_REQUEST, NULL, 4u,
                                                         &out) == AA_CONTROL_ERR_ARGUMENT);

    check("advertises sensor", aa_control_advertises(AA_CONTROL_CHANNEL_SENSOR));
    check("advertises navigation", aa_control_advertises(AA_CONTROL_CHANNEL_NAVIGATION));
    check("advertises input", aa_control_advertises(AA_CONTROL_CHANNEL_INPUT));
    check("does not advertise video", !aa_control_advertises(AA_CHANNEL_VIDEO));
    check("does not advertise control", !aa_control_advertises(AA_CHANNEL_CONTROL));

    check("state name ready", strcmp(aa_control_state_name(AA_CONTROL_READY), "ready") == 0);
    check("state name failed", strcmp(aa_control_state_name(AA_CONTROL_FAILED), "failed") == 0);
    check("status name parse", strcmp(aa_control_status_name(AA_CONTROL_ERR_PARSE), "parse") == 0);
}

int main(void)
{
    test_start_and_version();
    test_discovery();
    test_channel_open();
    test_ping_and_shutdown();
    test_errors_and_vocabulary();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_control: all checks passed\n");
    return 0;
}
