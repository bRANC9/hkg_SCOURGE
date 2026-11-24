/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the wireless Bluetooth bootstrap: the packet framing, the
 * fixed start and info-response vectors, the strict credential validation and
 * the five-stage state machine, including both framing readings.  No Bluetooth
 * stack and no phone are involved.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_btboot.h"

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

static void default_info(aa_btboot_info *info)
{
    memset(info, 0, sizeof *info);
    strcpy(info->ssid, "SCOURGE-AA");
    strcpy(info->key, "testpass123");
    strcpy(info->bssid, "DC:A6:32:E7:5A:FE");
    info->security_mode = AA_BTBOOT_SECURITY_WPA2_PERSONAL;
    info->ap_type = AA_BTBOOT_AP_DYNAMIC;
}

static void test_bssid(void)
{
    const uint8_t mac[6] = {0xDCu, 0xA6u, 0x32u, 0xE7u, 0x5Au, 0xFEu};
    char text[AA_BTBOOT_BSSID_LEN + 1u];
    check("bssid text", aa_btboot_bssid_text(mac, text));
    check("bssid text value", strcmp(text, "DC:A6:32:E7:5A:FE") == 0);

    uint8_t parsed[6];
    check("bssid parse", aa_btboot_bssid_parse(text, parsed) == AA_BTBOOT_OK);
    check("bssid roundtrip", memcmp(parsed, mac, 6) == 0);

    check("bssid short", aa_btboot_bssid_parse("DC:A6:32", parsed) == AA_BTBOOT_ERR_FIELD);
    check("bssid lowercase", aa_btboot_bssid_parse("dc:a6:32:e7:5a:fe", parsed) == AA_BTBOOT_ERR_FIELD);
    check("bssid separators", aa_btboot_bssid_parse("DC-A6-32-E7-5A-FE", parsed) == AA_BTBOOT_ERR_FIELD);
    check("bssid null", aa_btboot_bssid_parse(NULL, parsed) == AA_BTBOOT_ERR_ARGUMENT);
}

static void test_framing(void)
{
    uint8_t body[AA_BTBOOT_MAX_BODY];
    size_t body_len = hex_to_bytes("0a0931302e302e322e313010a829", body, sizeof body);
    uint8_t packet[AA_BTBOOT_MAX_PACKET];
    size_t packet_len = 0u;

    /* Length counts the message id and the body: 15 bytes for a 13-byte body. */
    check("pack", aa_btboot_pack(AA_BTBOOT_MSG_START_REQUEST, body, body_len, true, packet,
                                 sizeof packet, &packet_len) == AA_BTBOOT_OK);
    check("pack vector", bytes_are(packet, packet_len, "001000010a0931302e302e322e313010a829"));

    uint16_t message_id = 0u;
    const uint8_t *unpacked = NULL;
    size_t unpacked_len = 0u;
    size_t consumed = 0u;
    check("unpack", aa_btboot_unpack(packet, packet_len, true, &message_id, &unpacked,
                                     &unpacked_len, &consumed) == AA_BTBOOT_OK);
    check("unpack id", message_id == AA_BTBOOT_MSG_START_REQUEST);
    check("unpack body", unpacked_len == body_len && memcmp(unpacked, body, body_len) == 0);
    check("unpack consumed", consumed == packet_len);

    /* The other reading: the length covers only the body. */
    check("pack body-only", aa_btboot_pack(AA_BTBOOT_MSG_START_REQUEST, body, body_len, false,
                                           packet, sizeof packet, &packet_len) == AA_BTBOOT_OK);
    check("pack body-only vector", bytes_are(packet, packet_len, "000e00010a0931302e302e322e313010a829"));
    check("unpack body-only", aa_btboot_unpack(packet, packet_len, false, &message_id, &unpacked,
                                               &unpacked_len, &consumed) == AA_BTBOOT_OK);
    check("unpack body-only body", unpacked_len == body_len && memcmp(unpacked, body, body_len) == 0);
    check("unpack body-only consumed", consumed == packet_len);

    /* Reading the same bytes with the wrong convention is refused, not guessed. */
    uint8_t truncated[AA_BTBOOT_MAX_PACKET];
    memcpy(truncated, packet, packet_len - 1u);
    check("unpack truncated", aa_btboot_unpack(truncated, packet_len - 1u, false, &message_id,
                                               &unpacked, &unpacked_len, &consumed) ==
                                  AA_BTBOOT_ERR_LENGTH);
    check("unpack short buffer", aa_btboot_unpack(truncated, 3u, true, &message_id, &unpacked,
                                                  &unpacked_len, &consumed) == AA_BTBOOT_ERR_LENGTH);
    check("unpack zero length", aa_btboot_unpack(truncated, 2u, false, &message_id, &unpacked,
                                                 &unpacked_len, &consumed) == AA_BTBOOT_ERR_LENGTH);

    uint8_t tiny[4];
    check("pack small buffer", aa_btboot_pack(AA_BTBOOT_MSG_START_REQUEST, body, body_len, true,
                                              tiny, sizeof tiny, &packet_len) == AA_BTBOOT_ERR_BUFFER);
}

static void test_state_machine(void)
{
    aa_btboot boot;
    aa_btboot_info info;
    aa_btboot_start start;
    uint8_t packet[AA_BTBOOT_MAX_PACKET];
    uint8_t reply[AA_BTBOOT_MAX_PACKET];
    size_t packet_len = 0u;
    size_t reply_len = 0u;

    aa_btboot_reset(&boot);
    default_info(&info);
    memset(&start, 0, sizeof start);
    strcpy(start.ip, "10.0.2.10");
    start.port = 5288u;

    /* Credentials are required before the handshake starts. */
    check("begin without credentials", aa_btboot_begin(&boot, &start, packet, sizeof packet,
                                                       &packet_len) == AA_BTBOOT_ERR_STATE);
    check("credentials", aa_btboot_set_credentials(&boot, &info) == AA_BTBOOT_OK);

    check("begin", aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len) == AA_BTBOOT_OK);
    check("begin state", boot.state == AA_BTBOOT_START_SENT);
    check("begin packet", bytes_are(packet, packet_len, "001000010a0931302e302e322e313010a829"));
    check("begin twice", aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len) ==
                             AA_BTBOOT_ERR_STATE);

    /* Stage 2: the phone accepts. */
    uint8_t body[AA_BTBOOT_MAX_BODY];
    size_t body_len = hex_to_bytes("0800", body, sizeof body);
    check("start response", aa_btboot_on_packet(&boot, AA_BTBOOT_MSG_START_RESPONSE, body, body_len,
                                                reply, sizeof reply, &reply_len) == AA_BTBOOT_OK);
    check("start response state", boot.state == AA_BTBOOT_AWAIT_INFO && reply_len == 0u);

    /* Stage 3 and 4: the phone asks, we answer with the credentials. */
    check("info request", aa_btboot_on_packet(&boot, AA_BTBOOT_MSG_INFO_REQUEST, NULL, 0u, reply,
                                              sizeof reply, &reply_len) == AA_BTBOOT_OK);
    check("info response state", boot.state == AA_BTBOOT_INFO_SENT);
    check("info response packet header",
          reply_len > 4u && reply[0] == 0x00u && reply[1] == 0x32u && reply[2] == 0x00u &&
              reply[3] == 0x03u);
    check("info response ssid", bytes_are(reply + 4u, 12u, "0a0a53434f555247452d4141"));
    check("info response key", bytes_are(reply + 16u, 13u, "120b7465737470617373313233"));
    check("info response bssid", bytes_are(reply + 29u, 19u, "1a1144433a41363a33323a45373a35413a4645"));
    check("info response security", bytes_are(reply + 48u, 2u, "2008"));
    check("info response ap type", bytes_are(reply + 50u, 2u, "2801"));

    /* Stage 5: the phone joined the access point. */
    body_len = hex_to_bytes("0800", body, sizeof body);
    check("connect status", aa_btboot_on_packet(&boot, AA_BTBOOT_MSG_CONNECT_STATUS, body, body_len,
                                                reply, sizeof reply, &reply_len) == AA_BTBOOT_OK);
    check("connected", boot.state == AA_BTBOOT_CONNECTED);
}

static void test_refusals(void)
{
    aa_btboot boot;
    aa_btboot_info info;
    aa_btboot_start start;
    uint8_t packet[AA_BTBOOT_MAX_PACKET];
    uint8_t reply[AA_BTBOOT_MAX_PACKET];
    size_t packet_len = 0u;
    size_t reply_len = 0u;
    uint8_t body[AA_BTBOOT_MAX_BODY];

    aa_btboot_reset(&boot);
    default_info(&info);
    memset(&start, 0, sizeof start);
    strcpy(start.ip, "10.0.2.10");
    start.port = 5288u;
    (void)aa_btboot_set_credentials(&boot, &info);
    (void)aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len);

    /* The phone says no. */
    size_t body_len = hex_to_bytes("0801", body, sizeof body);
    check("refused start", aa_btboot_on_packet(&boot, AA_BTBOOT_MSG_START_RESPONSE, body, body_len,
                                               reply, sizeof reply, &reply_len) ==
                               AA_BTBOOT_ERR_REFUSED);
    check("refused state", boot.state == AA_BTBOOT_FAILED);

    /* Out of order messages are a state error, not a guess. */
    aa_btboot_reset(&boot);
    (void)aa_btboot_set_credentials(&boot, &info);
    check("info before start", aa_btboot_on_packet(&boot, AA_BTBOOT_MSG_INFO_REQUEST, NULL, 0u,
                                                   reply, sizeof reply, &reply_len) ==
                                   AA_BTBOOT_ERR_STATE);
    (void)aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len);
    check("connect before info", aa_btboot_on_packet(&boot, AA_BTBOOT_MSG_CONNECT_STATUS, NULL, 0u,
                                                     reply, sizeof reply, &reply_len) ==
                                     AA_BTBOOT_ERR_STATE);

    /* An unknown message id is tolerated and counted. */
    check("unknown message", aa_btboot_on_packet(&boot, 0x0055u, NULL, 0u, reply, sizeof reply,
                                                 &reply_len) == AA_BTBOOT_OK);
    check("unknown counted", boot.ignored == 1u);

    /* A status message without a status field is a field error. */
    body_len = hex_to_bytes("1001", body, sizeof body);
    check("start response without status",
          aa_btboot_on_packet(&boot, AA_BTBOOT_MSG_START_RESPONSE, body, body_len, reply,
                              sizeof reply, &reply_len) == AA_BTBOOT_ERR_FIELD);
}

static void test_credential_validation(void)
{
    aa_btboot boot;
    aa_btboot_info info;
    aa_btboot_reset(&boot);

    default_info(&info);
    info.key[0] = '\0';
    check("short key", aa_btboot_set_credentials(&boot, &info) == AA_BTBOOT_ERR_FIELD);

    default_info(&info);
    info.ssid[0] = '\0';
    check("empty ssid", aa_btboot_set_credentials(&boot, &info) == AA_BTBOOT_ERR_FIELD);

    default_info(&info);
    strcpy(info.bssid, "dc:a6:32:e7:5a:fe");
    check("lowercase bssid", aa_btboot_set_credentials(&boot, &info) == AA_BTBOOT_ERR_FIELD);

    default_info(&info);
    info.security_mode = 4u;
    check("wrong security", aa_btboot_set_credentials(&boot, &info) == AA_BTBOOT_ERR_FIELD);

    default_info(&info);
    info.ap_type = 5u;
    check("wrong ap type", aa_btboot_set_credentials(&boot, &info) == AA_BTBOOT_ERR_FIELD);

    default_info(&info);
    check("valid credentials", aa_btboot_set_credentials(&boot, &info) == AA_BTBOOT_OK);
    check("credentials flag", boot.credentials_set);

    /* Start address validation. */
    aa_btboot_start start;
    memset(&start, 0, sizeof start);
    strcpy(start.ip, "10.0.2.10");
    uint8_t packet[AA_BTBOOT_MAX_PACKET];
    size_t packet_len = 0u;
    start.port = 0u;
    aa_btboot_reset(&boot);
    (void)aa_btboot_set_credentials(&boot, &info);
    check("zero port", aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len) ==
                           AA_BTBOOT_ERR_FIELD);
    start.port = 5288u;
    strcpy(start.ip, "10.0.2.300.1.2");
    check("bad ip", aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len) ==
                        AA_BTBOOT_ERR_FIELD);
    strcpy(start.ip, "10.0.2.10");
    check("good ip", aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len) ==
                         AA_BTBOOT_OK);

    check("state names", strcmp(aa_btboot_state_name(AA_BTBOOT_CONNECTED), "connected") == 0);
    check("status names", strcmp(aa_btboot_status_name(AA_BTBOOT_ERR_REFUSED), "refused") == 0);
}

static void test_framing_switch(void)
{
    aa_btboot boot;
    aa_btboot_info info;
    aa_btboot_start start;
    uint8_t packet[AA_BTBOOT_MAX_PACKET];
    size_t packet_len = 0u;

    aa_btboot_reset(&boot);
    default_info(&info);
    memset(&start, 0, sizeof start);
    strcpy(start.ip, "10.0.2.10");
    start.port = 5288u;
    (void)aa_btboot_set_credentials(&boot, &info);

    aa_btboot_set_framing(&boot, false);
    check("framing begin", aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len) ==
                               AA_BTBOOT_OK);
    check("framing body-only packet",
          bytes_are(packet, packet_len, "000e00010a0931302e302e322e313010a829"));
}

int main(void)
{
    test_bssid();
    test_framing();
    test_state_machine();
    test_refusals();
    test_credential_validation();
    test_framing_switch();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_btboot: all checks passed\n");
    return 0;
}
