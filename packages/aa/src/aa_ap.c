/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * AP-to-bootstrap conversion.  See aa_ap.h: validation is delegated to
 * aa_btboot so both layers cannot drift apart.
 */
#include "aa_ap.h"

#include <string.h>

static bool copy_bounded(char *out, size_t out_cap, const char *text, size_t max_len)
{
    if (out == NULL || text == NULL)
        return false;
    const size_t len = strlen(text);
    if (len == 0u || len > max_len || len + 1u > out_cap)
        return false;
    memcpy(out, text, len + 1u);
    return true;
}

/* Parse a dotted quad without stdio: an engine module that pulls in printf
 * machinery for one parse is a bad trade. */
static bool parse_dotted_quad(const char *text, unsigned octets[4])
{
    if (text == NULL)
        return false;
    size_t index = 0u;
    for (size_t part = 0u; part < 4u; part++) {
        bool have_digit = false;
        unsigned value = 0u;
        while (text[index] >= '0' && text[index] <= '9') {
            value = value * 10u + (unsigned)(text[index] - '0');
            if (value > 255u)
                return false;
            have_digit = true;
            index++;
        }
        if (!have_digit)
            return false;
        octets[part] = value;
        if (part < 3u) {
            if (text[index] != '.')
                return false;
            index++;
        }
    }
    return text[index] == '\0';
}

/* Private ranges only: an AA AP on a public address would be a surprise, and a
 * bridge must never announce one. */
bool aa_ap_bssid_canonicalize(const char *text, char out[AA_BTBOOT_BSSID_LEN + 1u])
{
    static const char hex_upper[] = "0123456789ABCDEF";
    unsigned nibbles[12];
    size_t count = 0u;
    if (text == NULL || out == NULL)
        return false;

    for (size_t i = 0u; text[i] != '\0'; i++) {
        const char ch = text[i];
        if (ch == ':' || ch == '-')
            continue;
        int value = -1;
        if (ch >= '0' && ch <= '9')
            value = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            value = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
            value = ch - 'A' + 10;
        if (value < 0 || count == 12u)
            return false;
        nibbles[count++] = (unsigned)value;
    }
    if (count != 12u)
        return false;

    size_t at = 0u;
    for (size_t byte = 0u; byte < 6u; byte++) {
        out[at++] = hex_upper[nibbles[byte * 2u]];
        out[at++] = hex_upper[nibbles[byte * 2u + 1u]];
        if (byte != 5u)
            out[at++] = ':';
    }
    out[at] = '\0';
    return true;
}

static bool address_is_private(const char *address)
{
    unsigned octet[4] = {0u, 0u, 0u, 0u};
    if (!parse_dotted_quad(address, octet))
        return false;
    if (octet[0] == 10u)
        return true;
    if (octet[0] == 172u && octet[1] >= 16u && octet[1] <= 31u)
        return true;
    if (octet[0] == 192u && octet[1] == 168u)
        return true;
    return false;
}

aa_ap_status aa_ap_from_readback(const aa_ap_readback *readback, aa_ap_endpoint *out)
{
    if (readback == NULL || out == NULL)
        return AA_AP_ERR_ARGUMENT;
    if (!readback->on_air)
        return AA_AP_OFF_AIR;
    if (readback->port == 0u)
        return AA_AP_ERR_FIELD;
    if (!address_is_private(readback->address))
        return AA_AP_ERR_ADDRESS;

    aa_ap_endpoint result;
    memset(&result, 0, sizeof result);
    if (!copy_bounded(result.ssid, sizeof result.ssid, readback->ssid, AA_BTBOOT_MAX_SSID) ||
        !copy_bounded(result.key, sizeof result.key, readback->key, AA_BTBOOT_MAX_KEY) ||
        !copy_bounded(result.address, sizeof result.address, readback->address, AA_AP_MAX_ADDRESS))
        return AA_AP_ERR_FIELD;
    /* The AP layer reports the radio's MAC, not a canonical BSSID string. */
    if (!aa_ap_bssid_canonicalize(readback->bssid, result.bssid))
        return AA_AP_ERR_FIELD;
    result.port = readback->port;

    /* One source of truth for the credential rules. */
    aa_btboot_info info;
    memset(&info, 0, sizeof info);
    memcpy(info.ssid, result.ssid, sizeof info.ssid);
    memcpy(info.key, result.key, sizeof info.key);
    memcpy(info.bssid, result.bssid, sizeof info.bssid);
    info.security_mode = AA_BTBOOT_SECURITY_WPA2_PERSONAL;
    info.ap_type = AA_BTBOOT_AP_DYNAMIC;
    aa_btboot scratch;
    aa_btboot_reset(&scratch);
    if (aa_btboot_set_credentials(&scratch, &info) != AA_BTBOOT_OK)
        return AA_AP_ERR_FIELD;

    *out = result;
    return AA_AP_OK;
}

aa_ap_status aa_ap_btboot_info(const aa_ap_endpoint *endpoint, aa_btboot_info *out)
{
    if (endpoint == NULL || out == NULL)
        return AA_AP_ERR_ARGUMENT;
    aa_btboot_info info;
    memset(&info, 0, sizeof info);
    if (!copy_bounded(info.ssid, sizeof info.ssid, endpoint->ssid, AA_BTBOOT_MAX_SSID) ||
        !copy_bounded(info.key, sizeof info.key, endpoint->key, AA_BTBOOT_MAX_KEY))
        return AA_AP_ERR_FIELD;
    if (!aa_ap_bssid_canonicalize(endpoint->bssid, info.bssid))
        return AA_AP_ERR_FIELD;
    info.security_mode = AA_BTBOOT_SECURITY_WPA2_PERSONAL;
    info.ap_type = AA_BTBOOT_AP_DYNAMIC;
    *out = info;
    return AA_AP_OK;
}

aa_ap_status aa_ap_btboot_start(const aa_ap_endpoint *endpoint, aa_btboot_start *out)
{
    if (endpoint == NULL || out == NULL)
        return AA_AP_ERR_ARGUMENT;
    if (endpoint->port == 0u)
        return AA_AP_ERR_FIELD;
    if (!address_is_private(endpoint->address))
        return AA_AP_ERR_ADDRESS;
    aa_btboot_start start;
    memset(&start, 0, sizeof start);
    if (!copy_bounded(start.ip, sizeof start.ip, endpoint->address, AA_BTBOOT_MAX_IP))
        return AA_AP_ERR_FIELD;
    start.port = endpoint->port;
    *out = start;
    return AA_AP_OK;
}

const char *aa_ap_status_name(aa_ap_status status)
{
    switch (status) {
    case AA_AP_OK:
        return "ok";
    case AA_AP_ERR_ARGUMENT:
        return "argument";
    case AA_AP_OFF_AIR:
        return "off_air";
    case AA_AP_ERR_FIELD:
        return "field";
    case AA_AP_ERR_ADDRESS:
        return "address";
    case AA_AP_ERR_BUFFER:
        return "buffer";
    }
    return "unknown";
}
