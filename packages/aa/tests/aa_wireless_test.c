/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * End-to-end test of the wireless path with the real Wifi1b numbers: an AP
 * readback on 10.42.0.1 becomes the bootstrap packets, a scripted phone walks
 * the five RFCOMM stages, and the resulting AA session is driven to READY with
 * an EV push.  The TLS provider is the same deterministic stand-in the session
 * tests use; it models framing only and is not a cipher.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_wireless.h"

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

static bool bytes_are(const uint8_t *data, size_t len, const char *hex)
{
    uint8_t expected[AA_BTBOOT_MAX_PACKET];
    const size_t expected_len = hex_to_bytes(hex, expected, sizeof expected);
    return expected_len == len && memcmp(data, expected, len) == 0;
}

static void xor_bytes(uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; i++)
        data[i] = (uint8_t)(data[i] ^ 0x5Au);
}

/* ---- deterministic TLS provider (framing only, not a cipher) -------------- */

typedef struct {
    uint8_t pending[512];
    size_t pending_len;
    uint8_t incoming[512];
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

static aa_tls_provider fake_vtable(fake_provider *fake)
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

static aa_session_status feed_on(aa_wireless *wireless, uint8_t channel, const char *hex,
                                  bool encrypted, aa_session_out *out)
{
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    const size_t payload_len = hex_to_bytes(hex, payload, sizeof payload);
    uint8_t staged[AA_SESSION_MAX_PAYLOAD];
    memcpy(staged, payload, payload_len);
    if (encrypted)
        xor_bytes(staged, payload_len);
    uint8_t frame[AA_SESSION_MAX_FRAME];
    size_t frame_len = 0u;
    if (aa_wire_encode_message(channel, false, encrypted, staged, payload_len,
                               AA_WIRE_MAX_FRAME_PAYLOAD, frame, sizeof frame, &frame_len) !=
        AA_WIRE_OK)
        return AA_SESSION_ERR_WIRE;
    return aa_wireless_on_tcp(wireless, frame, frame_len, out);
}

static aa_session_status feed_hex(aa_wireless *wireless, const char *hex, bool encrypted,
                                  aa_session_out *out)
{
    return feed_on(wireless, (uint8_t)AA_CHANNEL_CONTROL, hex, encrypted, out);
}

/* Take the oldest frame, decrypting it, without judging it. */
static bool take_frame(aa_session *session, uint8_t *payload, size_t *payload_len, bool *encrypted)
{
    const uint8_t *data = NULL;
    size_t len = 0u;
    if (!aa_session_peek(session, &data, &len))
        return false;
    aa_wire_frame frame;
    if (aa_wire_parse_frame(data, len, &frame) != AA_WIRE_OK)
        return false;
    memcpy(payload, frame.payload, frame.payload_len);
    *payload_len = frame.payload_len;
    *encrypted = frame.encrypted;
    if (frame.encrypted)
        xor_bytes(payload, frame.payload_len);
    aa_session_consume(session);
    return true;
}

static bool expect_frame_on(aa_session *session, uint8_t channel, const char *hex,
                            bool encrypted)
{
    const uint8_t *data = NULL;
    size_t len = 0u;
    if (!aa_session_peek(session, &data, &len))
        return false;
    aa_wire_frame frame;
    if (aa_wire_parse_frame(data, len, &frame) != AA_WIRE_OK)
        return false;
    uint8_t payload[AA_SESSION_MAX_PAYLOAD];
    memcpy(payload, frame.payload, frame.payload_len);
    if (frame.encrypted)
        xor_bytes(payload, frame.payload_len);
    const bool matches = frame.channel == channel && frame.encrypted == encrypted &&
                         bytes_are(payload, frame.payload_len, hex);
    aa_session_consume(session);
    return matches;
}

static bool expect_frame(aa_session *session, const char *hex, bool encrypted)
{
    return expect_frame_on(session, (uint8_t)AA_CHANNEL_CONTROL, hex, encrypted);
}

static aa_ap_readback wifi1b_readback(void)
{
    aa_ap_readback ap;
    memset(&ap, 0, sizeof ap);
    ap.on_air = true;
    ap.ssid = "SCOURGE-AA";
    ap.key = "testpass123";
    ap.bssid = "DC:A6:32:E7:5A:FE";
    ap.address = "10.42.0.1";
    ap.port = AA_AP_DEFAULT_PORT;
    return ap;
}

static void test_full_wireless_path(void)
{
    static aa_wireless wireless; /* ~30 KiB: not on the stack */
    fake_provider fake;
    memset(&fake, 0, sizeof fake);
    aa_tls_provider provider = fake_vtable(&fake);
    const aa_ap_readback ap = wifi1b_readback();

    aa_wireless_reset(&wireless);
    uint8_t packet[AA_BTBOOT_MAX_PACKET];
    size_t packet_len = 0u;
    check("begin", aa_wireless_begin(&wireless, &ap, &provider, packet, sizeof packet, &packet_len) ==
                       AA_WIRELESS_OK);
    check("start packet", bytes_are(packet, packet_len,
                                    "001000010a0931302e34322e302e3110a829"));
    check("begin twice", aa_wireless_begin(&wireless, &ap, &provider, packet, sizeof packet,
                                           &packet_len) == AA_WIRELESS_ERR_STATE);
    check("not connected yet", !aa_wireless_ready_for_tcp(&wireless));

    uint8_t reply[AA_BTBOOT_MAX_PACKET];
    size_t reply_len = 0u;

    /* Stage 2: the phone accepts the start request. */
    uint8_t body[32];
    size_t body_len = hex_to_bytes("0800", body, sizeof body);
    uint8_t framed[AA_BTBOOT_MAX_PACKET];
    size_t framed_len = 0u;
    check("pack start response",
          aa_btboot_pack(AA_BTBOOT_MSG_START_RESPONSE, body, body_len, true, framed, sizeof framed,
                         &framed_len) == AA_BTBOOT_OK);
    check("start response",
          aa_wireless_on_bt_packet(&wireless, framed, framed_len, reply, sizeof reply, &reply_len) ==
              AA_WIRELESS_OK);
    check("no reply to start response", reply_len == 0u);

    /* Stage 3 and 4: the phone asks for the AP credentials. */
    framed_len = 0u;
    check("pack info request",
          aa_btboot_pack(AA_BTBOOT_MSG_INFO_REQUEST, NULL, 0u, true, framed, sizeof framed,
                         &framed_len) == AA_BTBOOT_OK);
    check("info request",
          aa_wireless_on_bt_packet(&wireless, framed, framed_len, reply, sizeof reply, &reply_len) ==
              AA_WIRELESS_OK);
    check("info reply present", reply_len > 4u && reply[2] == 0x00u && reply[3] == 0x03u);
    /* The credentials the phone receives are exactly the AP readback. */
    check("info reply ssid", bytes_are(reply + 4u, 12u, "0a0a53434f555247452d4141"));
    check("info reply bssid", bytes_are(reply + 29u, 19u, "1a1144433a41363a33323a45373a35413a4645"));

    /* Stage 5: the phone joined the AP; only now may TCP start. */
    body_len = hex_to_bytes("0800", body, sizeof body);
    framed_len = 0u;
    check("pack connect status",
          aa_btboot_pack(AA_BTBOOT_MSG_CONNECT_STATUS, body, body_len, true, framed, sizeof framed,
                         &framed_len) == AA_BTBOOT_OK);
    check("connect status",
          aa_wireless_on_bt_packet(&wireless, framed, framed_len, reply, sizeof reply, &reply_len) ==
              AA_WIRELESS_OK);
    check("ready for tcp", aa_wireless_ready_for_tcp(&wireless));
    check("session not started yet", !wireless.session_started);

    aa_session_out out;
    check("tcp before session",
          aa_wireless_on_tcp(&wireless, framed, framed_len, &out) == AA_SESSION_ERR_STATE);
    check("session start", aa_wireless_start_session(&wireless, &out) == AA_WIRELESS_OK);
    check("version request out", expect_frame(&wireless.session, "000100010000", false));

    /* From here it is an ordinary AA session, driven to READY. */
    check("version response", feed_hex(&wireless, "0002000100000000", false, &out) == AA_SESSION_OK);
    check("handshake frame", expect_frame(&wireless.session, "000348454c4c4f31", false));
    check("handshake", feed_hex(&wireless, "000348454c4c4f32", false, &out) == AA_SESSION_OK);
    check("handshake reply", expect_frame(&wireless.session, "000346494e4953484544", false));
    check("auth complete", expect_frame(&wireless.session, "00040800", false));
    check("discovery", feed_hex(&wireless, "0005220550686f6e65", true, &out) == AA_SESSION_OK);
    check("ready", wireless.session.state == AA_SESSION_READY && out.event == AA_CONTROL_EVENT_READY);

    /* The discovery reply is one frame; drain it before the sensor channel, or
     * every later expectation is off by one. */
    uint8_t drained[AA_SESSION_MAX_PAYLOAD];
    size_t drained_len = 0u;
    bool drained_encrypted = false;
    check("discovery reply drained",
          take_frame(&wireless.session, drained, &drained_len, &drained_encrypted) &&
              drained_encrypted && drained_len > 2u && drained[0] == 0x00u && drained[1] == 0x06u);

    /* The sensor channel, then the EV state of charge. */
    check("sensor open",
          feed_on(&wireless, (uint8_t)AA_CHANNEL_SENSOR, "000708001002", true, &out) ==
              AA_SESSION_OK);
    check("sensor open reply",
          expect_frame_on(&wireless.session, (uint8_t)AA_CHANNEL_SENSOR, "00080800", true));
    check("sensor start",
          feed_on(&wireless, (uint8_t)AA_CHANNEL_SENSOR, "8001081710e807", true, &out) ==
              AA_SESSION_OK);
    check("sensor start reply",
          expect_frame_on(&wireless.session, (uint8_t)AA_CHANNEL_SENSOR, "80020800", true));

    aa_sensor_vehicle vehicle;
    memset(&vehicle, 0, sizeof vehicle);
    vehicle.valid = true;
    vehicle.soc_permille = 505u;
    vehicle.capacity_wh = 80000u;
    vehicle.range_m = 300000u;
    vehicle.max_charge_power_w = 150000;
    vehicle.max_discharge_power_w = 150000;
    vehicle.regen_capable = true;
    check("vehicle push",
          aa_session_set_vehicle(aa_wireless_session(&wireless), &vehicle) == AA_SESSION_OK);
    check("ev event queued", aa_session_pending(aa_wireless_session(&wireless)) == 1u);
}

static void test_bad_readback(void)
{
    static aa_wireless wireless;
    fake_provider fake;
    memset(&fake, 0, sizeof fake);
    aa_tls_provider provider = fake_vtable(&fake);
    uint8_t packet[AA_BTBOOT_MAX_PACKET];
    size_t packet_len = 0u;

    aa_wireless_reset(&wireless);
    aa_ap_readback ap = wifi1b_readback();
    ap.on_air = false;
    check("off air", aa_wireless_begin(&wireless, &ap, &provider, packet, sizeof packet,
                                       &packet_len) == AA_WIRELESS_ERR_AP);

    ap = wifi1b_readback();
    ap.address = "8.8.8.8";
    check("public address", aa_wireless_begin(&wireless, &ap, &provider, packet, sizeof packet,
                                              &packet_len) == AA_WIRELESS_ERR_AP);

    /* A packet before begin is a state error, and a truncated one is framing. */
    aa_wireless_reset(&wireless);
    uint8_t reply[AA_BTBOOT_MAX_PACKET];
    size_t reply_len = 0u;
    uint8_t junk[8] = {0x00u, 0x04u, 0x00u, 0x07u, 0x08u, 0x00u, 0x00u, 0x00u};
    check("packet before begin",
          aa_wireless_on_bt_packet(&wireless, junk, sizeof junk, reply, sizeof reply, &reply_len) ==
              AA_WIRELESS_ERR_STATE);

    ap = wifi1b_readback();
    (void)aa_wireless_begin(&wireless, &ap, &provider, packet, sizeof packet, &packet_len);
    check("truncated packet",
          aa_wireless_on_bt_packet(&wireless, junk, 3u, reply, sizeof reply, &reply_len) ==
              AA_WIRELESS_ERR_FRAMING);
    check("status names", strcmp(aa_wireless_status_name(AA_WIRELESS_ERR_AP), "ap") == 0);
}

int main(void)
{
    test_full_wireless_path();
    test_bad_readback();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_wireless: all checks passed\n");
    return 0;
}
