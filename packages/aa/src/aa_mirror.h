/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * AA protocol as a *phone* — the mirror of the head-unit role.
 *
 * A bridge needs both shapes: head-unit-shaped toward the real phone (aa_session
 * does that) and phone-shaped toward the car's head unit.  This module is the
 * second one, and it is also the piece an in-car sniff test needs: connect it to
 * the head unit and it reports whether the unit accepts us as a phone, which
 * channels it advertises and whether it ever sends the EV energy model.
 *
 * Conversation it drives, from the head unit's first message:
 *
 *   HU -> version request (0x0001, raw u16 major/minor)
 *   HU <- version response (0x0002, raw u16 major/minor/status 0)
 *   HU -> auth complete (0x0004)                     [after the secure session]
 *   HU <- service discovery request (0x0005)         { device_name = 4, device_brand = 5 }
 *   HU -> service discovery response (0x0006)        -> which channels exist
 *   HU <- channel open request (0x0007)              one per wanted channel
 *   HU -> channel open response (0x0008)             { status = 1 }
 *   HU <- sensor start request (0x8001, sensor channel) { sensor_type = 1, refresh_interval = 2 }
 *   HU -> sensor event indication (0x8003)            -> EV energy model, field 23
 *   ping either way (0x000b / 0x000c)
 *
 * Facts pinned against aasdk commit 046b3b381595509d0939fa84b14a90978f46ff63
 * and the phone-side sequences in docs/22; nothing copied.  The EV field is the
 * undocumented sensor type 23 documented in aa_sensor.h.
 *
 * Local policy: one request is pending at a time (the caller sends what the
 * output describes and feeds the answer back), the wanted channels are sensor
 * and navigation when advertised, a refused channel open is counted instead of
 * ending the session, and an EV event is decoded only far enough to report the
 * current and maximum energy — the bridge's job is to relay, not to advise.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_MIRROR_H
#define AA_MIRROR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aa_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AA_MIRROR_MAX_PAYLOAD 512u
#define AA_MIRROR_DEVICE_NAME "SCOURGE bridge"
#define AA_MIRROR_DEVICE_BRAND "SCOURGE"
#define AA_MIRROR_SENSOR_INTERVAL_MS 1000u

typedef enum {
    AA_MIRROR_OK = 0,
    AA_MIRROR_ERR_ARGUMENT = -1,
    AA_MIRROR_ERR_STATE = -2,
    AA_MIRROR_ERR_CHANNEL = -3,
    AA_MIRROR_ERR_MESSAGE_ID = -4,
    AA_MIRROR_ERR_PARSE = -5,
    AA_MIRROR_ERR_BUFFER = -6,
    AA_MIRROR_ERR_VERSION = -7
} aa_mirror_status;

typedef enum {
    AA_MIRROR_IDLE = 0,
    AA_MIRROR_VERSION_ANSWERED = 1,
    AA_MIRROR_DISCOVERY_SENT = 2,
    AA_MIRROR_READY = 3,
    AA_MIRROR_FAILED = 4
} aa_mirror_state;

typedef enum {
    AA_MIRROR_EVENT_NONE = 0,
    AA_MIRROR_EVENT_READY = 1,   /* all wanted channels are open, sensors started */
    AA_MIRROR_EVENT_FAILED = 2,
    AA_MIRROR_EVENT_EV_MODEL = 3 /* the head unit sent an EV energy model */
} aa_mirror_event;

typedef struct {
    uint8_t payload[AA_MIRROR_MAX_PAYLOAD]; /* message id + body, ready for aa_wire */
    size_t payload_len;
    uint8_t channel;
    bool encrypted;
    aa_mirror_state state;
    aa_mirror_event event;
} aa_mirror_out;

typedef struct {
    aa_mirror_state state;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t hu_major;
    uint16_t hu_minor;
    bool secure;
    uint32_t advertised_mask; /* bit per channel id the head unit offered */
    uint32_t wanted_mask;     /* bit per channel id we ask for */
    uint32_t opened_mask;
    uint32_t refused_mask;
    bool sensor_started;
    bool ev_seen;
    int32_t ev_current_wh;
    int32_t ev_capacity_wh;
    uint32_t sensor_events;
    uint32_t ignored;
    uint32_t errors;
} aa_mirror;

void aa_mirror_reset(aa_mirror *mirror, uint16_t major, uint16_t minor);

/* The transport calls this once the secure session is up (as the phone, that
 * means the provider finished as TLS server). */
void aa_mirror_set_secure(aa_mirror *mirror, bool secure);

/* Consume one message from the head unit; the output describes what to send
 * back, if anything. */
aa_mirror_status aa_mirror_on_message(aa_mirror *mirror, uint8_t channel, uint16_t message_id,
                                      const uint8_t *body, size_t body_len, aa_mirror_out *out);

const char *aa_mirror_state_name(aa_mirror_state state);
const char *aa_mirror_status_name(aa_mirror_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_MIRROR_H */
