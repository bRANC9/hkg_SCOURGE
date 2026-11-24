/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Wireless Android Auto Bluetooth bootstrap (RFCOMM side).
 *
 * Before any Wi-Fi or TLS exists, the head unit tells the phone which endpoint
 * to dial and which access point to join.  That exchange runs over a Bluetooth
 * RFCOMM socket with its own tiny framing, and it is the first thing an in-car
 * test needs: without it the phone never starts a wireless session.
 *
 * Facts, pinned from the open-android-auto wireless setup guide (BlueZ 5.82,
 * documented 2024-02, cross-checked against aa-proxy-rs/bluer behaviour) and
 * recorded in docs/22 section 5b.  Documentation-derived, **not verified on
 * this unit**: treat a real capture as the authority.
 *
 *   packet  [length u16 BE][message id u16 BE][protobuf body]
 *   id 1    WifiStartRequest   head unit -> phone   ip_address = 1, port = 2
 *   id 7    WifiStartResponse  phone -> head unit   status = 1 (0 = success)
 *   id 2    WifiInfoRequest    phone -> head unit   normally empty
 *   id 3    WifiInfoResponse   head unit -> phone   see below
 *   id 6    WifiConnectStatus  phone -> head unit   status = 1 (0 = success)
 *
 *   WifiInfoResponse: ssid = 1, key = 2, bssid = 3 (length-delimited,
 *   colon-separated uppercase MAC), security_mode = 4 (WPA2_PERSONAL = 8),
 *   access_point_type = 5 (DYNAMIC = 1).
 *
 * Two deliberately explicit choices, because the guide does not settle them:
 *   - whether the length field counts the message id as well as the body.  This
 *     codec defaults to "yes" (2 + body) and exposes one boolean (see
 *     aa_btboot_set_framing) that flips every write and read at once;
 *   - the status field of ids 6 and 7 is read from field 1, which is inferred
 *     from the shape of the other messages rather than observed.
 * A capture replaces both guesses with facts; until then they are named here.
 *
 * Local policy: credentials are validated before they are announced (SSID
 * 1..32 bytes, WPA2 key 8..63, BSSID canonical uppercase), an unknown message
 * id is counted and tolerated, and state violations are refused instead of
 * guessed around.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_BTBOOT_H
#define AA_BTBOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AA_BTBOOT_HEADER_LEN 4u
#define AA_BTBOOT_MAX_BODY 256u
#define AA_BTBOOT_MAX_PACKET (AA_BTBOOT_HEADER_LEN + AA_BTBOOT_MAX_BODY)

#define AA_BTBOOT_MSG_START_REQUEST 1u
#define AA_BTBOOT_MSG_INFO_REQUEST 2u
#define AA_BTBOOT_MSG_INFO_RESPONSE 3u
#define AA_BTBOOT_MSG_CONNECT_STATUS 6u
#define AA_BTBOOT_MSG_START_RESPONSE 7u

#define AA_BTBOOT_SECURITY_WPA2_PERSONAL 8u
#define AA_BTBOOT_AP_STATIC 0u
#define AA_BTBOOT_AP_DYNAMIC 1u

#define AA_BTBOOT_MAX_IP 15u
#define AA_BTBOOT_MAX_SSID 32u
#define AA_BTBOOT_MAX_KEY 63u
#define AA_BTBOOT_MIN_KEY 8u
#define AA_BTBOOT_BSSID_LEN 17u

typedef enum {
    AA_BTBOOT_OK = 0,
    AA_BTBOOT_ERR_ARGUMENT = -1,
    AA_BTBOOT_ERR_STATE = -2,
    AA_BTBOOT_ERR_LENGTH = -3,
    AA_BTBOOT_ERR_BUFFER = -4,
    AA_BTBOOT_ERR_FIELD = -5,
    AA_BTBOOT_ERR_REFUSED = -6
} aa_btboot_status;

typedef enum {
    AA_BTBOOT_IDLE = 0,
    AA_BTBOOT_START_SENT = 1,
    AA_BTBOOT_AWAIT_INFO = 2,
    AA_BTBOOT_INFO_SENT = 3,
    AA_BTBOOT_CONNECTED = 4,
    AA_BTBOOT_FAILED = 5
} aa_btboot_state;

typedef struct {
    char ip[AA_BTBOOT_MAX_IP + 1u];
    uint32_t port;
} aa_btboot_start;

typedef struct {
    char ssid[AA_BTBOOT_MAX_SSID + 1u];
    char key[AA_BTBOOT_MAX_KEY + 1u];
    char bssid[AA_BTBOOT_BSSID_LEN + 1u];
    uint32_t security_mode;
    uint32_t ap_type;
} aa_btboot_info;

typedef struct {
    aa_btboot_state state;
    aa_btboot_start start;
    aa_btboot_info info;
    bool credentials_set;
    bool length_includes_message_id;
    uint32_t ignored;
    uint32_t errors;
} aa_btboot;

void aa_btboot_reset(aa_btboot *boot);

/* Credentials must be valid before the bootstrap can begin; a wrong SSID or key
 * would only fail later, on the phone, where it is harder to see. */
aa_btboot_status aa_btboot_set_credentials(aa_btboot *boot, const aa_btboot_info *info);

/* Framing. */
aa_btboot_status aa_btboot_pack(uint16_t message_id, const uint8_t *body, size_t body_len,
                                bool length_includes_message_id, uint8_t *out, size_t out_cap,
                                size_t *out_len);
aa_btboot_status aa_btboot_unpack(const uint8_t *buf, size_t len, bool length_includes_message_id,
                                  uint16_t *message_id, const uint8_t **body, size_t *body_len,
                                  size_t *consumed);

/* Change the framing reading for this session (default: the length counts the
 * message id).  Set it before aa_btboot_begin. */
void aa_btboot_set_framing(aa_btboot *boot, bool length_includes_message_id);

/* Emit the start request (stage 1) and move to START_SENT. */
aa_btboot_status aa_btboot_begin(aa_btboot *boot, const aa_btboot_start *start, uint8_t *packet,
                                 size_t packet_cap, size_t *packet_len);

/* Consume a peer message.  reply_len is 0 when nothing has to be sent, and at
 * most one reply packet is produced (the info response). */
aa_btboot_status aa_btboot_on_packet(aa_btboot *boot, uint16_t message_id, const uint8_t *body,
                                     size_t body_len, uint8_t *reply, size_t reply_cap,
                                     size_t *reply_len);

/* Canonical BSSID text ("DC:A6:32:E7:5A:FE") and its strict parser. */
bool aa_btboot_bssid_text(const uint8_t mac[6], char out[AA_BTBOOT_BSSID_LEN + 1u]);
aa_btboot_status aa_btboot_bssid_parse(const char *text, uint8_t mac[6]);

const char *aa_btboot_state_name(aa_btboot_state state);
const char *aa_btboot_status_name(aa_btboot_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_BTBOOT_H */
