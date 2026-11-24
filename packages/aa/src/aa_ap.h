/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The seam between an access-point layer and the AA wireless bootstrap.
 *
 * The Wifi1b package already owns the AP and its DHCP server, and the CarPlay
 * work already reads the AP state back (ssid, channel, security, link-local) in
 * `cp_boot_ap_readback`.  Android Auto needs the same information in a different
 * shape: the SSID, the PSK, the BSSID and the AP's IPv4 address have to be
 * turned into the two RFCOMM messages the phone expects (see aa_btboot.h).
 *
 * This module is that conversion and nothing else.  It deliberately takes plain
 * strings instead of including the Wifi1b headers, so the AA core stays
 * buildable and testable on its own and the AP layer does not have to know what
 * an AA bootstrap is.  Validation is not duplicated: every rule comes from
 * aa_btboot itself (SSID 1..32, WPA2 key 8..63, canonical uppercase BSSID,
 * usable IPv4 endpoint), so there is one place to get it wrong instead of two.
 *
 * What the AP layer must supply (three fields Wifi1b does not expose yet):
 *   - the BSSID in canonical text, i.e. what `wpa_cli status` reports as bssid=;
 *   - the AP's IPv4 address (Wifi1b's own WIFI1B_AP_IP, 10.42.0.1/24 today);
 *   - the TCP port this tree listens on, which is our announcement and not a
 *     protocol constant.
 * The SSID and the PSK are already in the AP configuration.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_AP_H
#define AA_AP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aa_btboot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AA_AP_MAX_ADDRESS 15u
/* Our announcement, not a wire fact: the phone dials whatever we send.  The
 * reference wireless setup used 5288. */
#define AA_AP_DEFAULT_PORT 5288u

typedef enum {
    AA_AP_OK = 0,
    AA_AP_ERR_ARGUMENT = -1,
    AA_AP_OFF_AIR = -2,
    AA_AP_ERR_FIELD = -3,
    AA_AP_ERR_ADDRESS = -4,
    AA_AP_ERR_BUFFER = -5
} aa_ap_status;

/* What an AP layer can hand over.  Any field may be NULL only when the caller
 * expects AA_AP_ERR_FIELD back. */
typedef struct {
    bool on_air;
    const char *ssid;
    const char *key;
    const char *bssid;   /* "DC:A6:32:E7:5A:FE" */
    const char *address; /* dotted-quad, private range */
    uint16_t port;
} aa_ap_readback;

typedef struct {
    char ssid[AA_BTBOOT_MAX_SSID + 1u];
    char key[AA_BTBOOT_MAX_KEY + 1u];
    char bssid[AA_BTBOOT_BSSID_LEN + 1u];
    char address[AA_AP_MAX_ADDRESS + 1u];
    uint16_t port;
} aa_ap_endpoint;

/* Canonicalise a MAC as the AP layer has it into the uppercase colon-separated
 * text the bootstrap must announce.  Accepts the forms an AP layer actually
 * produces: "/sys/class/net/wlan0/address" style lowercase, uppercase, bare
 * twelve hex digits, and '-' separators.  In AP mode the BSSID *is* the wlan0
 * MAC (measured on the unit as 28:0f:eb:02:21:8a), because a host access point
 * has no upstream bssid= to report. */
bool aa_ap_bssid_canonicalize(const char *text, char out[AA_BTBOOT_BSSID_LEN + 1u]);

/* Turn a readback into a validated endpoint.  Requires an on-air AP, a
 * private-range IPv4 address and a non-zero port. */
aa_ap_status aa_ap_from_readback(const aa_ap_readback *readback, aa_ap_endpoint *out);

/* The two bootstrap messages this endpoint produces. */
aa_ap_status aa_ap_btboot_info(const aa_ap_endpoint *endpoint, aa_btboot_info *out);
aa_ap_status aa_ap_btboot_start(const aa_ap_endpoint *endpoint, aa_btboot_start *out);

const char *aa_ap_status_name(aa_ap_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_AP_H */
