/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Android Auto channel and message-ID vocabulary shared by the wired and the
 * wireless transport.
 *
 * Wire facts (verified read-only against aasdk commit
 * 046b3b381595509d0939fa84b14a90978f46ff63; nothing is copied):
 *   - the frame channel byte is the channel ID itself;
 *   - the first two payload bytes of a channel message are a big-endian
 *     message ID, and the rest of the payload is that message's body;
 *   - control-channel message IDs are 0x0001..0x0013 as listed below.
 *
 * The channel numbers are the same on a USB session and on a wireless one, so
 * every consumer of this header is transport-agnostic.  Diagnostic-only
 * helpers live here instead of a table in the engine so that a future
 * protocol layer can be checked against one list.
 */

#ifndef AA_CHANNELS_H
#define AA_CHANNELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AA_CHANNEL_CONTROL = 0,
    AA_CHANNEL_INPUT = 1,
    AA_CHANNEL_SENSOR = 2,
    AA_CHANNEL_VIDEO = 3,
    AA_CHANNEL_MEDIA_AUDIO = 4,
    AA_CHANNEL_SPEECH_AUDIO = 5,
    AA_CHANNEL_SYSTEM_AUDIO = 6,
    AA_CHANNEL_AV_INPUT = 7,
    AA_CHANNEL_BLUETOOTH = 8,
    /* Navigation is not part of the reference channel enum: the head unit
     * assigns its number in the service-discovery descriptor.  This tree uses
     * 9 for it, and the descriptor in aa_control.h declares the same value, so
     * the assignment is self-consistent even though it is a local choice. */
    AA_CHANNEL_NAVIGATION = 9,
    AA_CHANNEL_UNKNOWN = 255
} aa_channel_id;

/* Control-channel message IDs. */
#define AA_CONTROL_VERSION_REQUEST 0x0001u
#define AA_CONTROL_VERSION_RESPONSE 0x0002u
#define AA_CONTROL_SSL_HANDSHAKE 0x0003u
#define AA_CONTROL_AUTH_COMPLETE 0x0004u
#define AA_CONTROL_SERVICE_DISCOVERY_REQUEST 0x0005u
#define AA_CONTROL_SERVICE_DISCOVERY_RESPONSE 0x0006u
#define AA_CONTROL_CHANNEL_OPEN_REQUEST 0x0007u
#define AA_CONTROL_CHANNEL_OPEN_RESPONSE 0x0008u
#define AA_CONTROL_PING_REQUEST 0x000Bu
#define AA_CONTROL_PING_RESPONSE 0x000Cu
#define AA_CONTROL_NAVIGATION_FOCUS_REQUEST 0x000Du
#define AA_CONTROL_NAVIGATION_FOCUS_RESPONSE 0x000Eu
#define AA_CONTROL_SHUTDOWN_REQUEST 0x000Fu
#define AA_CONTROL_SHUTDOWN_RESPONSE 0x0010u
#define AA_CONTROL_VOICE_SESSION_REQUEST 0x0011u
#define AA_CONTROL_AUDIO_FOCUS_REQUEST 0x0012u
#define AA_CONTROL_AUDIO_FOCUS_RESPONSE 0x0013u

/* True for a channel this tree knows how to name.  Unknown channel bytes are
 * not an error at the frame layer: a session must tolerate a channel it does
 * not implement instead of dropping the transport. */
bool aa_channel_known(uint8_t channel);

/* Static name for diagnostics; never returns NULL.  Unknown values map to
 * "unknown" and are not an error. */
const char *aa_channel_name(uint8_t channel);

/* Feature areas this tree intends to implement on top of the frame layer.  The
 * list exists so that transports do not have to agree on a string. */
typedef enum {
    AA_FEATURE_EV_ENERGY = 0,  /* vehicle energy model toward the phone */
    AA_FEATURE_NAVIGATION = 1, /* turn-by-turn from the NAVIGATION data */
    AA_FEATURE_VIDEO = 2,
    AA_FEATURE_AUDIO = 3,
    AA_FEATURE_INPUT = 4
} aa_feature;

const char *aa_feature_name(aa_feature feature);

#ifdef __cplusplus
}
#endif

#endif /* AA_CHANNELS_H */
