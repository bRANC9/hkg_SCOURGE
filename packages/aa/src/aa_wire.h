/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded, allocation-free codec for the Android Auto frame layer.
 *
 * This is the transport-agnostic half of the AA core: the same frame layer
 * runs over USB AOAP (wired) and over TLS/TCP after the wireless bootstrap,
 * so nothing here may assume a transport.  Only framing is implemented in
 * this checkpoint; protobuf payloads, TLS and the channel state machines are
 * later checkpoints.
 *
 * Wire facts implemented (verified read-only against aasdk commit
 * 046b3b381595509d0939fa84b14a90978f46ff63; no source is copied, vendored or
 * derived from that tree, and this implementation is independent):
 *
 *   - a frame is [channel][flags][2-byte big-endian frame payload length]
 *     followed by exactly that many payload bytes;
 *   - a FIRST frame (frame-type bits exactly 0x01) replaces the 2-byte length
 *     with a 6-byte field: 2-byte frame payload length, then a 4-byte
 *     big-endian total message length;
 *   - only a FIRST frame uses the extended field.  MIDDLE (0x00), LAST (0x02)
 *     and BULK (0x03) frames use the short one, and BULK means "this frame is
 *     the whole message";
 *   - flag bits are 0x01 FIRST, 0x02 LAST, 0x04 CONTROL, 0x08 ENCRYPTED.
 *     Bits 0x10-0x80 are not part of the protocol and are rejected here;
 *   - every frame of one message carries the same channel, CONTROL and
 *     ENCRYPTED bits; only the frame-type bits vary;
 *   - the first two payload bytes of a channel message are a big-endian
 *     message ID (see aa_channels.h).
 *
 * Local policy, not wire facts: AA_WIRE_MAX_FRAME_PAYLOAD mirrors the
 * reference sender's 0x4000 fragmentation threshold, AA_WIRE_MAX_MESSAGE is a
 * caller-buffer sanity bound, and this encoder never emits an empty trailing
 * LAST frame while the decoder accepts one, because the reference sender can
 * produce it when a message is exactly one chunk long.
 *
 * This module never allocates, logs, reads configuration or performs I/O.
 * Frames of other layers may carry credentials, so callers must not log
 * frames they did not construct themselves.
 *
 * Streaming contract of aa_wire_decode_message():
 *   - the decoder state lives in the caller's aa_wire_decode and survives
 *     calls;
 *   - *consumed counts complete frames only, so a partially received frame is
 *     never consumed and the caller keeps those bytes for the next call;
 *   - AA_WIRE_END means "a complete frame was consumed, the message is not
 *     finished yet, feed more bytes"; AA_WIRE_ERR_TRUNCATED means no complete
 *     frame was available at all (including an empty buffer);
 *   - any error resets the decoder state, so the caller must resynchronise by
 *     dropping the connection-level stream, not by re-feeding the same bytes.
 */

#ifndef AA_WIRE_H
#define AA_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AA_WIRE_HEADER_LEN 2u
#define AA_WIRE_SIZE_SHORT_LEN 2u
#define AA_WIRE_SIZE_EXTENDED_LEN 6u

/* Flag bits of the second header byte. */
#define AA_WIRE_FLAG_FIRST 0x01u
#define AA_WIRE_FLAG_LAST 0x02u
#define AA_WIRE_FLAG_BULK (AA_WIRE_FLAG_FIRST | AA_WIRE_FLAG_LAST)
#define AA_WIRE_FLAG_CONTROL 0x04u
#define AA_WIRE_FLAG_ENCRYPTED 0x08u
#define AA_WIRE_FLAG_FRAME_TYPE_MASK 0x03u
#define AA_WIRE_FLAG_MASK 0x0Fu

/* Local bounds.  AA_WIRE_MAX_FRAME_PAYLOAD is the reference sender's
 * fragmentation threshold; a larger frame payload is rejected rather than
 * buffered.  AA_WIRE_MAX_MESSAGE bounds a reassembled message; a real video
 * frame fits well below it. */
#define AA_WIRE_MAX_FRAME_PAYLOAD 0x4000u
#define AA_WIRE_MAX_MESSAGE (4u * 1024u * 1024u)

typedef enum {
    AA_WIRE_OK = 0,
    AA_WIRE_END = 1,
    AA_WIRE_ERR_ARGUMENT = -1,
    AA_WIRE_ERR_TRUNCATED = -2,
    AA_WIRE_ERR_FLAGS = -3,
    AA_WIRE_ERR_LENGTH = -4,
    AA_WIRE_ERR_CHANNEL = -5,
    AA_WIRE_ERR_INTERLEAVED = -6,
    AA_WIRE_ERR_OVERSIZE = -7
} aa_wire_status;

typedef struct {
    uint8_t channel;
    uint8_t flags;
    bool control;
    bool encrypted;
    bool first;
    bool last;
    /* Payload bytes of this frame, and their count. */
    const uint8_t *payload;
    size_t payload_len;
    /* Total message length; meaningful only for a FIRST frame. */
    uint32_t total_len;
    /* Bytes this frame occupies in the stream: header + size field + payload. */
    size_t frame_len;
} aa_wire_frame;

typedef struct {
    bool active;
    uint8_t channel;
    bool control;
    bool encrypted;
    uint32_t total;
    size_t received;
} aa_wire_decode;

typedef struct {
    uint8_t channel;
    bool control;
    bool encrypted;
    const uint8_t *payload;
    size_t payload_len;
} aa_wire_message;

/* Parse exactly one frame from the start of buf.  A frame that does not fit in
 * len is AA_WIRE_ERR_TRUNCATED and consumes nothing. */
aa_wire_status aa_wire_parse_frame(const uint8_t *buf, size_t len, aa_wire_frame *out);

/* Encode one frame.  flags must be a legal combination: FIRST requires a
 * nonzero total_len that is at least payload_len, every other frame type
 * requires total_len == 0.  payload_len must not exceed
 * AA_WIRE_MAX_FRAME_PAYLOAD.  On a too-small output buffer the function
 * returns AA_WIRE_ERR_OVERSIZE and reports no length. */
aa_wire_status aa_wire_encode_frame(uint8_t channel, uint8_t flags, uint32_t total_len,
                                    const uint8_t *payload, size_t payload_len,
                                    uint8_t *out, size_t out_cap, size_t *out_len);

/* Encode one whole message, fragmenting when payload_len exceeds chunk.  chunk
 * must be 1..AA_WIRE_MAX_FRAME_PAYLOAD; payload_len must not exceed
 * AA_WIRE_MAX_MESSAGE.  A message shorter than or equal to chunk is one BULK
 * frame.  An empty payload is legal at this layer: a channel message's first
 * two bytes are its message ID, so message validity is a later checkpoint. */
aa_wire_status aa_wire_encode_message(uint8_t channel, bool control, bool encrypted,
                                      const uint8_t *payload, size_t payload_len,
                                      size_t chunk, uint8_t *out, size_t out_cap,
                                      size_t *out_len);

/* Reset the streaming decoder.  Must be called before the first use. */
void aa_wire_decode_init(aa_wire_decode *state);

/* Decode frames from buf until one whole message is assembled into msg.  msg
 * must hold at least the message's total length; a message larger than msg_cap
 * is AA_WIRE_ERR_OVERSIZE and the state is reset.  out->payload points into
 * msg. */
aa_wire_status aa_wire_decode_message(aa_wire_decode *state, const uint8_t *buf, size_t len,
                                      uint8_t *msg, size_t msg_cap,
                                      aa_wire_message *out, size_t *consumed);

/* Optional diagnostics: a static string, never frame contents. */
const char *aa_wire_status_name(aa_wire_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_WIRE_H */
