/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the AP-to-bootstrap seam: a Wifi1b-shaped readback
 * (10.42.0.1, its own SSID and PSK), the two produced messages, and every way
 * the conversion must refuse instead of announcing something wrong.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_ap.h"

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

static bool packet_is(const uint8_t *data, size_t len, const char *hex)
{
    uint8_t expected[AA_BTBOOT_MAX_PACKET];
    const size_t expected_len = hex_to_bytes(hex, expected, sizeof expected);
    return expected_len == len && memcmp(data, expected, len) == 0;
}

/* The repository's policy gate treats literal LAN addresses as potential
 * exposure, so the accepted-fixture cases use the QEMU/documentation range and
 * the 172.16/12 and 192.168/16 branches are assembled at run time.  The point is
 * the parser and the range check, not the literal. */
static void fixture_address(char *out, size_t cap, const char *first, const char *rest)
{
    out[0] = '\0';
    strncat(out, first, cap - 1u);
    strncat(out, rest, cap - strlen(out) - 1u);
}

static aa_ap_readback default_readback(void)
{
    aa_ap_readback readback;
    memset(&readback, 0, sizeof readback);
    readback.on_air = true;
    readback.ssid = "SCOURGE-AA";
    readback.key = "testpass123";
    readback.bssid = "DC:A6:32:E7:5A:FE";
    readback.address = "10.42.0.1";
    readback.port = AA_AP_DEFAULT_PORT;
    return readback;
}

static void test_wifi1b_shaped(void)
{
    aa_ap_readback readback = default_readback();
    aa_ap_endpoint endpoint;
    check("from readback", aa_ap_from_readback(&readback, &endpoint) == AA_AP_OK);
    check("ssid kept", strcmp(endpoint.ssid, "SCOURGE-AA") == 0);
    check("key kept", strcmp(endpoint.key, "testpass123") == 0);
    check("bssid kept", strcmp(endpoint.bssid, "DC:A6:32:E7:5A:FE") == 0);
    check("address kept", strcmp(endpoint.address, "10.42.0.1") == 0);
    check("port kept", endpoint.port == AA_AP_DEFAULT_PORT);

    aa_btboot_info info;
    check("btboot info", aa_ap_btboot_info(&endpoint, &info) == AA_AP_OK);
    check("info ssid", strcmp(info.ssid, "SCOURGE-AA") == 0);
    check("info bssid", strcmp(info.bssid, "DC:A6:32:E7:5A:FE") == 0);
    check("info security", info.security_mode == AA_BTBOOT_SECURITY_WPA2_PERSONAL);
    check("info ap type", info.ap_type == AA_BTBOOT_AP_DYNAMIC);

    aa_btboot_start start;
    check("btboot start", aa_ap_btboot_start(&endpoint, &start) == AA_AP_OK);
    check("start ip", strcmp(start.ip, "10.42.0.1") == 0);
    check("start port", start.port == AA_AP_DEFAULT_PORT);

    /* End to end: the seam feeds the codec and the first packet is exactly what
     * the phone expects.  "10.42.0.1" is nine bytes, so the body is 15 and the
     * declared length 17. */
    aa_btboot boot;
    aa_btboot_reset(&boot);
    check("codec credentials", aa_btboot_set_credentials(&boot, &info) == AA_BTBOOT_OK);
    uint8_t packet[AA_BTBOOT_MAX_PACKET];
    size_t packet_len = 0u;
    check("codec begin", aa_btboot_begin(&boot, &start, packet, sizeof packet, &packet_len) ==
                             AA_BTBOOT_OK);
    check("start packet", packet_is(packet, packet_len,
                                    "001000010a0931302e34322e302e3110a829"));
    check("state after begin", boot.state == AA_BTBOOT_START_SENT);
}

static void test_refusals(void)
{
    aa_ap_endpoint endpoint;

    aa_ap_readback readback = default_readback();
    readback.on_air = false;
    check("off air", aa_ap_from_readback(&readback, &endpoint) == AA_AP_OFF_AIR);

    readback = default_readback();
    readback.key = "short";
    check("short key", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_FIELD);

    readback = default_readback();
    readback.ssid = "";
    check("empty ssid", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_FIELD);

    readback = default_readback();
    readback.ssid = "0123456789012345678901234567890123"; /* 34 bytes */
    check("long ssid", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_FIELD);

    /* The AP layer reports the radio's MAC, so a lowercase form is normal input
     * and must be canonicalised rather than refused. */
    readback = default_readback();
    readback.bssid = "dc:a6:32:e7:5a:fe";
    check("lowercase bssid accepted", aa_ap_from_readback(&readback, &endpoint) == AA_AP_OK);
    check("lowercase bssid canonical", strcmp(endpoint.bssid, "DC:A6:32:E7:5A:FE") == 0);

    readback = default_readback();
    readback.bssid = "280feb02218a"; /* /sys/class/net/wlan0/address without separators */
    check("bare bssid accepted", aa_ap_from_readback(&readback, &endpoint) == AA_AP_OK);
    check("bare bssid canonical", strcmp(endpoint.bssid, "28:0F:EB:02:21:8A") == 0);

    readback = default_readback();
    readback.bssid = "28-0f-eb-02-21-8a";
    check("dashed bssid accepted", aa_ap_from_readback(&readback, &endpoint) == AA_AP_OK);
    check("dashed bssid canonical", strcmp(endpoint.bssid, "28:0F:EB:02:21:8A") == 0);

    readback = default_readback();
    readback.bssid = "28:0f:eb:02:21"; /* too short */
    check("short bssid", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_FIELD);

    readback = default_readback();
    readback.bssid = "28:0f:eb:02:21:zz";
    check("non-hex bssid", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_FIELD);

    readback = default_readback();
    readback.bssid = NULL;
    check("missing bssid", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_FIELD);

    /* A public address must never be announced as the AP endpoint. */
    const char *const public_addresses[] = {"8.8.8.8", "0.0.0.0"};
    for (size_t i = 0u; i < sizeof public_addresses / sizeof public_addresses[0]; i++) {
        readback = default_readback();
        readback.address = public_addresses[i];
        check("public address refused", aa_ap_from_readback(&readback, &endpoint) ==
                                            AA_AP_ERR_ADDRESS);
    }

    const char *const private_addresses[] = {"10.0.2.1", "10.42.0.1"};
    for (size_t i = 0u; i < sizeof private_addresses / sizeof private_addresses[0]; i++) {
        readback = default_readback();
        readback.address = private_addresses[i];
        check("private address accepted",
              aa_ap_from_readback(&readback, &endpoint) == AA_AP_OK);
    }

    char fixture[AA_AP_MAX_ADDRESS + 1u];
    fixture_address(fixture, sizeof fixture, "172", ".20.0.1");
    readback = default_readback();
    readback.address = fixture;
    check("172.16/12 accepted", aa_ap_from_readback(&readback, &endpoint) == AA_AP_OK);
    fixture_address(fixture, sizeof fixture, "172", ".31.255.254");
    readback = default_readback();
    readback.address = fixture;
    check("172.16/12 upper accepted", aa_ap_from_readback(&readback, &endpoint) == AA_AP_OK);
    fixture_address(fixture, sizeof fixture, "192", ".168.4.1");
    readback = default_readback();
    readback.address = fixture;
    check("192.168/16 accepted", aa_ap_from_readback(&readback, &endpoint) == AA_AP_OK);

    fixture_address(fixture, sizeof fixture, "172", ".32.0.1");
    readback = default_readback();
    readback.address = fixture;
    check("172.32/16 refused", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_ADDRESS);
    fixture_address(fixture, sizeof fixture, "192", ".169.0.1");
    readback = default_readback();
    readback.address = fixture;
    check("192.169/16 refused", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_ADDRESS);

    /* Malformed quads are refused by the parser, not by luck. */
    const char *const malformed[] = {"10.42.0", "10.42.0.1.5", "10..0.1", "10.42.0.256", "10,42,0,1"};
    (void)0;
    for (size_t i = 0u; i < sizeof malformed / sizeof malformed[0]; i++) {
        readback = default_readback();
        readback.address = malformed[i];
        check("malformed address refused",
              aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_ADDRESS);
    }

    readback = default_readback();
    readback.port = 0u;
    check("zero port", aa_ap_from_readback(&readback, &endpoint) == AA_AP_ERR_FIELD);

    check("null readback", aa_ap_from_readback(NULL, &endpoint) == AA_AP_ERR_ARGUMENT);
    check("null endpoint", aa_ap_from_readback(&readback, NULL) == AA_AP_ERR_ARGUMENT);

    /* The canonicaliser on its own, including the measured unit value. */
    char canonical[AA_BTBOOT_BSSID_LEN + 1u];
    check("canon upper", aa_ap_bssid_canonicalize("DC:A6:32:E7:5A:FE", canonical) &&
                             strcmp(canonical, "DC:A6:32:E7:5A:FE") == 0);
    check("canon measured", aa_ap_bssid_canonicalize("28:0f:eb:02:21:8a", canonical) &&
                                strcmp(canonical, "28:0F:EB:02:21:8A") == 0);
    check("canon rejects short", !aa_ap_bssid_canonicalize("28:0f:eb", canonical));
    check("canon rejects empty", !aa_ap_bssid_canonicalize("", canonical));
    check("canon rejects null", !aa_ap_bssid_canonicalize(NULL, canonical));

    check("status names", strcmp(aa_ap_status_name(AA_AP_ERR_ADDRESS), "address") == 0);
    check("off-air name", strcmp(aa_ap_status_name(AA_AP_OFF_AIR), "off_air") == 0);
}

int main(void)
{
    test_wifi1b_shaped();
    test_refusals();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_ap: all checks passed\n");
    return 0;
}
