/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Control-channel state machine of the AA core.
 *
 * The state machine is transport-agnostic and clock-free: it consumes decoded
 * control messages (channel byte, message ID, body) and produces the reply
 * payload to send, or nothing at all.  The caller owns the wire, the TLS
 * upgrade and the clocks; this module owns only what the protocol says next.
 *
 * Message shapes implemented here, pinned against aasdk commit
 * 046b3b381595509d0939fa84b14a90978f46ff63 (read-only, nothing copied):
 *
 *   version request/response   raw big-endian u16 triples, not protobuf:
 *                              major, minor(, status).  Our request carries
 *                              major and minor only; the phone's response adds
 *                              status, where 0 is a match and 0xFFFF a
 *                              mismatch.
 *   auth complete              AuthCompleteIndication { status = 1 }, where 0
 *                              is OK and 1 is FAIL.
 *   service discovery request  ServiceDiscoveryRequest { device_name = 4,
 *                              device_brand = 5 }; unknown fields are skipped.
 *   service discovery response ServiceDiscoveryResponse { channels = 1,
 *                              head_unit_name = 2, car_model = 3,
 *                              car_year = 4, car_serial = 5,
 *                              left_hand_drive_vehicle = 6,
 *                              headunit_manufacturer = 7, headunit_model = 8,
 *                              sw_build = 9, sw_version = 10,
 *                              can_play_native_media_during_vr = 11,
 *                              hide_clock = 12 }.
 *                              ChannelDescriptor { channel_id = 1,
 *                              sensor_channel = 2, av_channel = 3,
 *                              input_channel = 4, av_input_channel = 5,
 *                              bluetooth_channel = 6, navigation_channel = 8,
 *                              vendor_extension_channel = 12 }.
 *                              SensorChannel { sensors = 1 } of
 *                              Sensor { type = 1 }.  NavigationChannel {
 *                              minimum_interval_ms = 1, type = 2,
 *                              image_options = 3 } of NavigationImageOptions {
 *                              width = 1, height = 2, colour_depth_bits = 3 }.
 *                              InputChannel { supported_keycodes = 1,
 *                              touch_screen_config = 2, touch_pad_config = 3 }
 *                              of TouchConfig { width = 1, height = 2 }.
 *   channel open request       ChannelOpenRequest { priority = 1,
 *                              channel_id = 2 }.
 *   channel open response      ChannelOpenResponse { status = 1 }.
 *   ping request/response      PingRequest/PingResponse { timestamp = 1 }.
 *   shutdown request/response  ShutdownRequest { reason = 1 } is accepted
 *                              without decoding the reason;
 *                              ShutdownResponse is empty.
 *
 * Advertised channels (local capability declaration, not a wire fact): sensor
 * 2 with gear, car speed, parking brake, driving status, night data and the EV
 * energy model (sensor type 23); navigation 9 (our assignment, see
 * aa_channels.h) with a 480x480 icon size; input 1 with a 1280x720 touch
 * surface and no keycodes yet.  Video and audio channels are deliberately
 * absent until a decoder and an audio path exist, so a session opened against
 * this checkpoint carries sensor and navigation data only.
 *
 * Error policy: a message that is valid but not implemented is counted in
 * `ignored` and never fails the session; a malformed message returns
 * AA_CONTROL_ERR_PARSE and leaves the state alone, so the transport decides
 * whether the session dies.  A rejected version is terminal (AA_CONTROL_FAILED)
 * and returns AA_CONTROL_ERR_VERSION.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_CONTROL_H
#define AA_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aa_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AA_CONTROL_MAX_PAYLOAD 1024u
#define AA_CONTROL_MAX_DEVICE_NAME 32u

/* Identity strings advertised in the service-discovery response.  They are
 * product names only; nothing here identifies an owner or a vehicle. */
#define AA_CONTROL_HEAD_UNIT_NAME "SCOURGE AA"
#define AA_CONTROL_CAR_MODEL "Ioniq 5"
#define AA_CONTROL_HEAD_UNIT_MANUFACTURER "SCOURGE"
#define AA_CONTROL_HEAD_UNIT_MODEL "aa-core"
#define AA_CONTROL_SW_BUILD "aa4"
#define AA_CONTROL_SW_VERSION "0.1.0"

/* Advertised channel numbers. */
#define AA_CONTROL_CHANNEL_SENSOR ((uint8_t)AA_CHANNEL_SENSOR)
#define AA_CONTROL_CHANNEL_NAVIGATION ((uint8_t)AA_CHANNEL_NAVIGATION)
#define AA_CONTROL_CHANNEL_INPUT ((uint8_t)AA_CHANNEL_INPUT)

typedef enum {
    AA_CONTROL_OK = 0,
    AA_CONTROL_ERR_ARGUMENT = -1,
    AA_CONTROL_ERR_CHANNEL = -2,
    AA_CONTROL_ERR_STATE = -3,
    AA_CONTROL_ERR_MESSAGE_ID = -4,
    AA_CONTROL_ERR_PARSE = -5,
    AA_CONTROL_ERR_BUFFER = -6,
    AA_CONTROL_ERR_VERSION = -7
} aa_control_status;

typedef enum {
    AA_CONTROL_IDLE = 0,
    AA_CONTROL_VERSION_SENT = 1,
    AA_CONTROL_TLS_PENDING = 2,
    AA_CONTROL_AUTH_SENT = 3,
    AA_CONTROL_READY = 4,
    AA_CONTROL_SHUTDOWN = 5,
    AA_CONTROL_FAILED = 6
} aa_control_state;

typedef enum {
    AA_CONTROL_EVENT_NONE = 0,
    AA_CONTROL_EVENT_READY = 1,
    AA_CONTROL_EVENT_SHUTDOWN = 2,
    AA_CONTROL_EVENT_FAILED = 3
} aa_control_event;

typedef struct {
    /* A complete channel message payload: 2-byte big-endian message ID, then
     * the body.  Ready to hand to aa_wire_encode_message on the control
     * channel. */
    uint8_t payload[AA_CONTROL_MAX_PAYLOAD];
    size_t payload_len;
    aa_control_state state;
    aa_control_event event;
} aa_control_out;

typedef struct {
    aa_control_state state;
    uint16_t major;
    uint16_t minor;
    uint16_t peer_major;
    uint16_t peer_minor;
    bool version_matched;
    uint32_t opened_mask; /* bit per advertised channel number */
    char device_name[AA_CONTROL_MAX_DEVICE_NAME];
    size_t device_name_len;
    uint32_t replied;
    uint32_t ignored;
    uint32_t errors;
} aa_control;

void aa_control_reset(aa_control *control, uint16_t major, uint16_t minor);
void aa_control_out_reset(aa_control_out *out);

/* IDLE -> VERSION_SENT, emitting the version request. */
aa_control_status aa_control_start(aa_control *control, aa_control_out *out);

/* TLS_PENDING -> AUTH_SENT, emitting the auth-complete indication.  The
 * transport calls this once the TLS layer (a later checkpoint) is up, because
 * auth complete must not be sent in the clear. */
aa_control_status aa_control_on_tls_established(aa_control *control, aa_control_out *out);

/* Consume one decoded control message.  body is the payload after the message
 * ID. */
aa_control_status aa_control_on_message(aa_control *control, uint8_t channel, uint16_t message_id,
                                        const uint8_t *body, size_t body_len, aa_control_out *out);

/* True when this tree advertises the channel, i.e. when a channel-open request
 * for it should be accepted. */
bool aa_control_advertises(uint8_t channel);

const char *aa_control_state_name(aa_control_state state);
const char *aa_control_status_name(aa_control_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_CONTROL_H */
