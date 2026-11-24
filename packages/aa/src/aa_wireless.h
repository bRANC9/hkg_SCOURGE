/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The wireless path as one object: access-point readback in, AA session out.
 *
 * This is the piece that ties the parts together without owning a socket, a
 * Bluetooth stack or a clock.  It takes what an AP layer knows (see aa_ap.h),
 * turns it into the RFCOMM bootstrap (aa_btboot), and once the phone reports
 * that it joined the access point it hands the caller a live AA session in the
 * head-unit role (aa_session).  A transport layer therefore only has to move
 * bytes: RFCOMM bytes in and out, then TCP bytes in and out.
 *
 * Order it enforces:
 *   begin            AP readback -> the WifiStartRequest packet the phone needs
 *   on_bt_packet     phone's response, its info request (we answer with the AP
 *                    credentials), then its connect status
 *   start_session    after the phone is on the AP: our version request
 *   on_tcp           everything from there is ordinary AA framing
 *
 * Local policy: the session is not created before the phone reports success, a
 * second begin is refused, and packets are only accepted in the order the
 * bootstrap expects.  Socket ownership, BlueZ registration, the AP itself and
 * the real TLS provider stay outside, which is what keeps this testable offline
 * with the real Wifi1b numbers.
 *
 * Nothing here allocates or performs I/O; the embedded session makes this
 * object large (about 30 KiB), so it belongs in a static or heap allocation,
 * not on a small stack.
 */

#ifndef AA_WIRELESS_H
#define AA_WIRELESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aa_ap.h"
#include "aa_btboot.h"
#include "aa_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AA_WIRELESS_OK = 0,
    AA_WIRELESS_ERR_ARGUMENT = -1,
    AA_WIRELESS_ERR_STATE = -2,
    AA_WIRELESS_ERR_AP = -3,
    AA_WIRELESS_ERR_BOOTSTRAP = -4,
    AA_WIRELESS_ERR_FRAMING = -5,
    AA_WIRELESS_ERR_SESSION = -6
} aa_wireless_status;

typedef struct {
    bool began;
    bool phone_connected;
    aa_ap_endpoint endpoint;
    aa_btboot bootstrap;
    aa_session session;
    bool session_started;
} aa_wireless;

void aa_wireless_reset(aa_wireless *wireless);

/* Turn the AP readback into the first bootstrap packet. */
aa_wireless_status aa_wireless_begin(aa_wireless *wireless, const aa_ap_readback *ap,
                                     const aa_tls_provider *provider, uint8_t *packet,
                                     size_t packet_cap, size_t *packet_len);

/* One RFCOMM packet from the phone.  The reply (if any) is a complete packet
 * ready to write back; reply_len is 0 when nothing has to be sent. */
aa_wireless_status aa_wireless_on_bt_packet(aa_wireless *wireless, const uint8_t *data, size_t len,
                                            uint8_t *reply, size_t reply_cap, size_t *reply_len);

/* True once the phone reported that it joined the AP. */
bool aa_wireless_ready_for_tcp(const aa_wireless *wireless);

/* Open the AA session: our version request goes out on the TCP side. */
aa_wireless_status aa_wireless_start_session(aa_wireless *wireless, aa_session_out *out);

/* Feed TCP bytes into the session and collect what it wants to send. */
aa_session_status aa_wireless_on_tcp(aa_wireless *wireless, const uint8_t *data, size_t len,
                                    aa_session_out *out);

/* The session, so the caller can push vehicle state or peek outgoing frames. */
aa_session *aa_wireless_session(aa_wireless *wireless);

const char *aa_wireless_status_name(aa_wireless_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_WIRELESS_H */
