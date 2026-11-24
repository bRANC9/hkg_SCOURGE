/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * End-to-end tests for the session glue with a scripted phone and a
 * deterministic provider.  The provider XORs every byte with 0x5A: it models
 * record framing and reversibility only, never confidentiality, and it exists
 * solely so the negotiation can be driven without a TLS library.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_proto.h"
#include "../src/aa_session.h"
#include "../src/aa_wire.h"

#define FAKE_KEY 0x5Au

static int g_failures;

static void check(const char *name, bool ok)
{
    if (!ok) {
        g_failures++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

static void xor_bytes(uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; i++)
        data[i] = (uint8_t)(data[i] ^ FAKE_KEY);
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

static bool hex_eq(const uint8_t *data, size_t len, const char *hex)
{
    uint8_t expected[512];
    const size_t expected_len = hex_to_bytes(hex, expected, sizeof expected);
    return expected_len == len && memcmp(data, expected, len) == 0;
}

/* ---- deterministic provider ---------------------------------------------- */

typedef struct {
    uint8_t pending[256];
    size_t pending_len;
    uint8_t incoming[256];
    size_t incoming_len;
    int phase;
} fake_provider;

static aa_tls_status fake_reset(void *context)
{
    fake_provider *fake = context;
    fake->pending_len = 0u;
    fake->incoming_len = 0u;
    fake->phase = 0;
    return AA_TLS_OK;
}

static aa_tls_status fake_handshake(void *context)
{
    fake_provider *fake = context;
    if (fake->phase == 0) {
        memcpy(fake->pending, "HELLO1", 6u);
        fake->pending_len = 6u;
        fake->phase = 1;
        return AA_TLS_OK;
    }
    if (fake->phase == 1 && fake->incoming_len != 0u) {
        memcpy(fake->pending, "FINISHED", 8u);
        fake->pending_len = 8u;
        fake->incoming_len = 0u;
        fake->phase = 2;
        return AA_TLS_ESTABLISHED;
    }
    fake->pending_len = 0u;
    return AA_TLS_OK;
}

static aa_tls_status fake_feed(void *context, const uint8_t *data, size_t len)
{
    fake_provider *fake = context;
    if (len > sizeof fake->incoming)
        return AA_TLS_ERR_BUFFER;
    memcpy(fake->incoming, data, len);
    fake->incoming_len = len;
    return AA_TLS_OK;
}

static aa_tls_status fake_take(void *context, uint8_t *out, size_t out_cap, size_t *out_len)
{
    fake_provider *fake = context;
    if (fake->pending_len > out_cap) {
        *out_len = 0u;
        return AA_TLS_ERR_BUFFER;
    }
    memcpy(out, fake->pending, fake->pending_len);
    *out_len = fake->pending_len;
    fake->pending_len = 0u;
    return AA_TLS_OK;
}

static aa_tls_status fake_write(void *context, const uint8_t *data, size_t len)
{
    fake_provider *fake = context;
    if (len > sizeof fake->pending)
        return AA_TLS_ERR_BUFFER;
    memcpy(fake->pending, data, len);
    xor_bytes(fake->pending, len);
    fake->pending_len = len;
    return AA_TLS_OK;
}

static aa_tls_status fake_read(void *context, uint8_t *out, size_t out_cap, size_t *out_len)
{
    fake_provider *fake = context;
    if (fake->incoming_len > out_cap) {
        *out_len = 0u;
        return AA_TLS_ERR_BUFFER;
    }
    memcpy(out, fake->incoming, fake->incoming_len);
    xor_bytes(out, fake->incoming_len);
    *out_len = fake->incoming_len;
    fake->incoming_len = 0u;
    return AA_TLS_OK;
}

static aa_tls_provider fake_provider_vtable(fake_provider *fake)
{
    aa_tls_provider provider;
    provider.context = fake;
    provider.reset = fake_reset;
    provider.handshake = fake_handshake;
    provider.feed = fake_feed;
    provider.take = fake_take;
    provider.write = fake_write;
    provider.read = fake_read;
    return provider;
}

/* ---- scripted phone helpers ---------------------------------------------- */

static size_t phone_frame_on(uint8_t *out, size_t out_cap, uint8_t channel,
                            const uint8_t *payload, size_t payload_len, bool encrypted)
{
    uint8_t staged[AA_SESSION_MAX_PAYLOAD];
    if (payload_len > sizeof staged)
        return 0u;
    memcpy(staged, payload, payload_len);
    if (encrypted)
        xor_bytes(staged, payload_len);
    size_t frame_len = 0u;
    if (aa_wire_encode_message(channel, false, encrypted, staged, payload_len,
                               AA_WIRE_MAX_FRAME_PAYLOAD, out, out_cap, &frame_len) != AA_WIRE_OK)
        return 0u;
    return frame_len;
}

static size_t phone_frame(uint8_t *out, size_t out_cap, const uint8_t *payload, size_t payload_len,
                          bool encrypted)
{
    return phone_frame_on(out, out_cap, (uint8_t)AA_CHANNEL_CONTROL, payload, payload_len,
                          encrypted);
}

/* Build a phone frame on a given channel from hex and feed it. */
static aa_session_status feed_on(aa_session *session, uint8_t channel, const char *hex,
                                 bool encrypted, aa_session_out *out)
{
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    const size_t payload_len = hex_to_bytes(hex, payload, sizeof payload);
    uint8_t frame[AA_SESSION_MAX_FRAME];
    const size_t frame_len =
        phone_frame_on(frame, sizeof frame, channel, payload, payload_len, encrypted);
    return aa_session_feed(session, frame, frame_len, out);
}

static aa_session_status feed_hex(aa_session *session, const char *hex, bool encrypted,
                                  aa_session_out *out)
{
    return feed_on(session, (uint8_t)AA_CHANNEL_CONTROL, hex, encrypted, out);
}

/* Take the oldest queued frame, returning its channel and (decrypted) payload. */
static bool take_frame_on(aa_session *session, uint8_t *channel, uint8_t *payload,
                          size_t *payload_len, bool *encrypted)
{
    const uint8_t *data = NULL;
    size_t len = 0u;
    if (!aa_session_peek(session, &data, &len))
        return false;
    aa_wire_frame frame;
    if (aa_wire_parse_frame(data, len, &frame) != AA_WIRE_OK)
        return false;
    *channel = frame.channel;
    memcpy(payload, frame.payload, frame.payload_len);
    *payload_len = frame.payload_len;
    *encrypted = frame.encrypted;
    if (frame.encrypted)
        xor_bytes(payload, *payload_len);
    aa_session_consume(session);
    return true;
}

static bool take_frame(aa_session *session, uint8_t *payload, size_t *payload_len,
                       bool *encrypted)
{
    uint8_t channel = 0u;
    return take_frame_on(session, &channel, payload, payload_len, encrypted) &&
           channel == (uint8_t)AA_CHANNEL_CONTROL;
}

/* Expect a frame on a specific channel with an exact payload. */
static bool expect_frame_on(aa_session *session, uint8_t channel, const char *hex, bool encrypted)
{
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    size_t payload_len = 0u;
    bool frame_encrypted = false;
    uint8_t frame_channel = 0u;
    if (!take_frame_on(session, &frame_channel, payload, &payload_len, &frame_encrypted))
        return false;
    return frame_channel == channel && frame_encrypted == encrypted &&
           hex_eq(payload, payload_len, hex);
}

static bool expect_frame(aa_session *session, const char *hex, bool encrypted)
{
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    size_t payload_len = 0u;
    bool frame_encrypted = false;
    if (!take_frame(session, payload, &payload_len, &frame_encrypted))
        return false;
    return frame_encrypted == encrypted && hex_eq(payload, payload_len, hex);
}

static void drive_to_handshake(aa_session *session, aa_session_out *out)
{
    (void)aa_session_start(session, out);
    (void)expect_frame(session, "000100010000", false);
    (void)feed_hex(session, "0002000100000000", false, out);
    (void)expect_frame(session, "000348454c4c4f31", false);
}

/* Version match, handshake, auth and a minimal discovery request: READY. */
static void drive_to_ready(aa_session *session, aa_session_out *out)
{
    drive_to_handshake(session, out);
    (void)feed_hex(session, "000348454c4c4f32", false, out);
    (void)expect_frame(session, "000346494e4953484544", false);
    (void)expect_frame(session, "00040800", false);
    (void)feed_hex(session, "000522055068667265", true, out);
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    size_t payload_len = 0u;
    bool encrypted = false;
    (void)take_frame(session, payload, &payload_len, &encrypted);
}

static void test_sensor_channel(void)
{
    fake_provider fake;
    memset(&fake, 0, sizeof fake);
    aa_tls_provider provider = fake_provider_vtable(&fake);
    aa_session session;
    aa_session_out out;
    (void)aa_session_reset(&session, &provider, 1u, 0u);
    drive_to_ready(&session, &out);

    check("ready before sensor", session.state == AA_SESSION_READY);

    /* The service channel repeats the open handshake on its own channel. */
    check("sensor open",
          feed_on(&session, (uint8_t)AA_CHANNEL_SENSOR, "000708001002", true, &out) ==
              AA_SESSION_OK);
    check("sensor open reply",
          expect_frame_on(&session, (uint8_t)AA_CHANNEL_SENSOR, "00080800", true));
    check("sensor open flagged", session.sensor.channel_open_answered);

    /* Vehicle state before the phone asks for it must not be sent. */
    aa_sensor_vehicle vehicle;
    memset(&vehicle, 0, sizeof vehicle);
    vehicle.valid = true;
    vehicle.soc_permille = 505u;
    vehicle.capacity_wh = 80000u;
    vehicle.range_m = 300000u;
    vehicle.max_charge_power_w = 150000;
    vehicle.max_discharge_power_w = 150000;
    vehicle.regen_capable = true;
    check("vehicle before request", aa_session_set_vehicle(&session, &vehicle) == AA_SESSION_OK);
    check("nothing queued before request", aa_session_pending(&session) == 0u);

    /* Sensor start request for the EV energy model. */
    check("sensor start",
          feed_on(&session, (uint8_t)AA_CHANNEL_SENSOR, "8001081710e807", true, &out) ==
              AA_SESSION_OK);
    check("sensor start reply",
          expect_frame_on(&session, (uint8_t)AA_CHANNEL_SENSOR, "80020800", true));
    check("sensor ev requested", session.sensor.ev_energy_requested && session.sensor.started);

    /* Now the state of charge reaches the phone. */
    check("vehicle push", aa_session_set_vehicle(&session, &vehicle) == AA_SESSION_OK);
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    size_t payload_len = 0u;
    bool encrypted = false;
    uint8_t channel = 0u;
    check("sensor event present",
          take_frame_on(&session, &channel, payload, &payload_len, &encrypted));
    check("sensor event channel", channel == (uint8_t)AA_CHANNEL_SENSOR);
    check("sensor event encrypted", encrypted);
    check("sensor event id", payload_len > 2u && payload[0] == 0x80u && payload[1] == 0x03u);
    check("sensor event ev field", payload_len > 4u && payload[2] == 0xBAu && payload[3] == 0x01u);

    /* A simple value event uses the same channel. */
    check("gear event", aa_session_send_sensor_value(&session, AA_SENSOR_VALUE_GEAR, 101) ==
                             AA_SESSION_OK);
    check("gear event frame",
          expect_frame_on(&session, (uint8_t)AA_CHANNEL_SENSOR, "800342020865", true));

    check("queue drained", aa_session_pending(&session) == 0u);
}

/* ---- tests --------------------------------------------------------------- */

static void test_full_negotiation(void)
{
    fake_provider fake;
    memset(&fake, 0, sizeof fake);
    aa_tls_provider provider = fake_provider_vtable(&fake);
    aa_session session;
    aa_session_out out;

    check("reset", aa_session_reset(&session, &provider, 1u, 0u) == AA_SESSION_OK);
    check("start", aa_session_start(&session, &out) == AA_SESSION_OK);
    check("start state", out.state == AA_SESSION_VERSION_SENT);
    check("version request frame", expect_frame(&session, "000100010000", false));
    check("queue empty", aa_session_pending(&session) == 0u);
    check("start twice", aa_session_start(&session, &out) == AA_SESSION_ERR_STATE);

    /* Version response: the session must start the secure session itself. */
    check("version response", feed_hex(&session, "0002000100000000", false, &out) == AA_SESSION_OK);
    check("version response state", out.state == AA_SESSION_HANDSHAKE);
    check("handshake frame", expect_frame(&session, "000348454c4c4f31", false));

    /* The peer finishes its side: our last handshake record plus auth complete. */
    check("handshake", feed_hex(&session, "000348454c4c4f32", false, &out) == AA_SESSION_OK);
    check("handshake state", out.state == AA_SESSION_AUTH_SENT);
    check("handshake reply", expect_frame(&session, "000346494e4953484544", false));
    check("auth complete", expect_frame(&session, "00040800", false));

    /* Discovery request is encrypted, and so is its response. */
    check("discovery", feed_hex(&session, "0005220550686f6e65", true, &out) == AA_SESSION_OK);
    check("discovery state", out.state == AA_SESSION_READY && out.event == AA_CONTROL_EVENT_READY);
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    size_t payload_len = 0u;
    bool encrypted = false;
    check("discovery reply present", take_frame(&session, payload, &payload_len, &encrypted));
    check("discovery reply encrypted", encrypted);
    check("discovery reply id", payload_len > 2u && payload[0] == 0x00u && payload[1] == 0x06u);

    /* Count the advertised channels in the decrypted response. */
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, payload + 2u, payload_len - 2u);
    size_t channel_count = 0u;
    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        if (aa_proto_next(&iter, &field, &wire) != AA_PROTO_OK)
            break;
        if (field == 1u && wire == AA_PROTO_WIRE_LEN)
            channel_count++;
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            break;
    }
    check("discovery reply channels", channel_count == 3u);

    check("channel open", feed_hex(&session, "000708001002", true, &out) == AA_SESSION_OK);
    check("channel open reply", expect_frame(&session, "00080800", true));

    check("ping", feed_hex(&session, "000b08ac02", true, &out) == AA_SESSION_OK);
    check("ping reply", expect_frame(&session, "000c08ac02", true));

    check("shutdown", feed_hex(&session, "000f0801", true, &out) == AA_SESSION_OK);
    check("shutdown state", out.state == AA_SESSION_SHUTDOWN && out.event == AA_CONTROL_EVENT_SHUTDOWN);
    check("shutdown reply", expect_frame(&session, "0010", true));
    check("final queue empty", aa_session_pending(&session) == 0u);
}

static void test_early_encryption_is_fatal(void)
{
    fake_provider fake;
    memset(&fake, 0, sizeof fake);
    aa_tls_provider provider = fake_provider_vtable(&fake);
    aa_session session;
    aa_session_out out;
    (void)aa_session_reset(&session, &provider, 1u, 0u);
    (void)aa_session_start(&session, &out);
    (void)expect_frame(&session, "000100010000", false);

    /* An encrypted frame before the secure session exists must never reach the
     * control state machine. */
    check("early encryption", feed_hex(&session, "0005220550686f6e65", true, &out) == AA_SESSION_ERR_TLS);
    check("early encryption state", session.state == AA_SESSION_FAILED && out.state == AA_SESSION_FAILED);
}

static void test_handshake_bounds(void)
{
    fake_provider fake;
    memset(&fake, 0, sizeof fake);
    aa_tls_provider provider = fake_provider_vtable(&fake);
    aa_session session;
    aa_session_out out;

    /* Oversized handshake body. */
    (void)aa_session_reset(&session, &provider, 1u, 0u);
    drive_to_handshake(&session, &out);
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    memset(payload, 0x41, sizeof payload);
    const size_t body_len = AA_TLS_MAX_HANDSHAKE_BLOB + 1u;
    payload[0] = 0x00u;
    payload[1] = 0x03u;
    uint8_t frame[AA_SESSION_MAX_FRAME];
    const size_t frame_len = phone_frame(frame, sizeof frame, payload, body_len + 2u, false);
    check("oversize handshake",
          frame_len != 0u && aa_session_feed(&session, frame, frame_len, &out) == AA_SESSION_ERR_TLS);
    check("oversize handshake state", session.state == AA_SESSION_FAILED);

    /* Handshake flood stops at the round bound. */
    memset(&fake, 0, sizeof fake);
    provider = fake_provider_vtable(&fake);
    (void)aa_session_reset(&session, &provider, 1u, 0u);
    drive_to_handshake(&session, &out);
    aa_session_status status = AA_SESSION_OK;
    for (uint32_t round = 0u; round <= AA_TLS_MAX_ROUNDS && status == AA_SESSION_OK; round++)
        status = feed_hex(&session, "000341", false, &out);
    check("handshake flood", status == AA_SESSION_ERR_TLS && session.state == AA_SESSION_FAILED);
}

static void test_partial_and_foreign_frames(void)
{
    fake_provider fake;
    memset(&fake, 0, sizeof fake);
    aa_tls_provider provider = fake_provider_vtable(&fake);
    aa_session session;
    aa_session_out out;

    /* A frame split across two reads decodes once the rest arrives. */
    (void)aa_session_reset(&session, &provider, 1u, 0u);
    (void)aa_session_start(&session, &out);
    (void)expect_frame(&session, "000100010000", false);

    uint8_t payload[8];
    size_t payload_len = hex_to_bytes("0002000100000000", payload, sizeof payload);
    uint8_t frame[64];
    const size_t frame_len = phone_frame(frame, sizeof frame, payload, payload_len, false);

    check("partial feed", aa_session_feed(&session, frame, 3u, &out) == AA_SESSION_OK);
    check("partial consumed nothing", out.consumed == 0u && out.state == AA_SESSION_VERSION_SENT);
    check("partial then rest",
          aa_session_feed(&session, frame, frame_len, &out) == AA_SESSION_OK);
    check("partial completes", out.consumed == frame_len && out.state == AA_SESSION_HANDSHAKE);
    check("partial handshake", expect_frame(&session, "000348454c4c4f31", false));

    /* A frame on a channel this checkpoint does not implement is counted, not
     * fatal (video/audio are advertised by nothing yet but the phone may try). */
    uint8_t video_payload[4] = {0x08u, 0x00u, 0x10u, 0x03u};
    uint8_t video_frame[32];
    size_t video_len = 0u;
    check("video frame build",
          aa_wire_encode_message((uint8_t)AA_CHANNEL_VIDEO, false, false, video_payload,
                                 sizeof video_payload, AA_WIRE_MAX_FRAME_PAYLOAD, video_frame,
                                 sizeof video_frame, &video_len) == AA_WIRE_OK);
    check("foreign channel", aa_session_feed(&session, video_frame, video_len, &out) == AA_SESSION_OK);
    check("foreign channel counted", session.ignored_channels == 1u && out.state == AA_SESSION_HANDSHAKE);
}

int main(void)
{
    test_full_negotiation();
    test_sensor_channel();
    test_early_encryption_is_fatal();
    test_handshake_bounds();
    test_partial_and_foreign_frames();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_session: all checks passed\n");
    return 0;
}
