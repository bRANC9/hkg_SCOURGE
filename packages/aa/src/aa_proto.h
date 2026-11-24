/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded, allocation-free protobuf codec for the Android Auto messages this
 * tree implements.
 *
 * Only the wire format is implemented: varints, length-delimited fields and
 * the fixed-width types that have to be skipped.  No schema, no reflection, no
 * generated code, no dependency on a protobuf runtime.  Field numbers and
 * message shapes live with their consumer (aa_control.h), keep their own
 * comments and are pinned there.
 *
 * Local policy, not wire facts:
 *   - a varint longer than 10 bytes or one whose tenth byte exceeds 0x01 is
 *     rejected as AA_PROTO_ERR_VARINT;
 *   - fixed-width values are read and written little-endian, as protobuf defines
  them, and only `float` is exposed as a typed writer because a 32-bit float
  is the only fixed-width value this tree sends;
 *   - group wire types (3, 4) and the reserved types (6, 7) are rejected as
 *     AA_PROTO_ERR_WIRE instead of being skipped: this tree never sends them;
 *   - a string field must be valid UTF-8 and must fit the caller's buffer with
 *     room for a NUL, otherwise it is a hard error.  Truncating a device name
 *     silently would misreport what the phone called itself;
 *   - the writer fails closed: the first overflow poisons the writer and every
 *     later call returns AA_PROTO_ERR_OVERFLOW, so a caller can build a
 *     message with a single check at the end.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_PROTO_H
#define AA_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AA_PROTO_MAX_VARINT_BYTES 10u

typedef enum {
    AA_PROTO_OK = 0,
    AA_PROTO_END = 1,
    AA_PROTO_ERR_ARGUMENT = -1,
    AA_PROTO_ERR_VARINT = -2,
    AA_PROTO_ERR_WIRE = -3,
    AA_PROTO_ERR_TRUNCATED = -4,
    AA_PROTO_ERR_OVERFLOW = -5,
    AA_PROTO_ERR_UTF8 = -6
} aa_proto_status;

/* Wire types this tree reads.  They are protocol values, not local names. */
#define AA_PROTO_WIRE_VARINT 0u
#define AA_PROTO_WIRE_FIXED64 1u
#define AA_PROTO_WIRE_LEN 2u
#define AA_PROTO_WIRE_FIXED32 5u

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t offset;
} aa_proto_iter;

void aa_proto_iter_init(aa_proto_iter *iter, const uint8_t *data, size_t len);

/* Advance to the next field key.  AA_PROTO_END means the message ended
 * exactly.  The caller reads the value with the reader matching wire_type or
 * skips it with aa_proto_skip. */
aa_proto_status aa_proto_next(aa_proto_iter *iter, uint32_t *field_number, uint8_t *wire_type);

aa_proto_status aa_proto_read_varint(aa_proto_iter *iter, uint64_t *value);

/* Fixed-width values (wire types 5 and 1), little-endian as protobuf defines
 * them.  The caller has already read the field key. */
aa_proto_status aa_proto_read_fixed32(aa_proto_iter *iter, uint32_t *value);
aa_proto_status aa_proto_read_fixed64(aa_proto_iter *iter, uint64_t *value);

/* Length-delimited value: the returned pointer stays inside the caller's
 * buffer and the iterator has already advanced past it. */
aa_proto_status aa_proto_read_len(aa_proto_iter *iter, const uint8_t **data, size_t *len);

/* Length-delimited UTF-8 string, NUL-terminated in out.  out_cap must have room
 * for the text plus the terminator. */
aa_proto_status aa_proto_read_string(aa_proto_iter *iter, char *out, size_t out_cap,
                                     size_t *out_len);

aa_proto_status aa_proto_skip(aa_proto_iter *iter, uint8_t wire_type);

bool aa_proto_utf8_valid(const char *text, size_t len);

const char *aa_proto_status_name(aa_proto_status status);

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool failed;
} aa_proto_writer;

void aa_proto_writer_init(aa_proto_writer *writer, uint8_t *buf, size_t cap);
bool aa_proto_writer_ok(const aa_proto_writer *writer);
size_t aa_proto_writer_len(const aa_proto_writer *writer);

aa_proto_status aa_proto_put_varint_field(aa_proto_writer *writer, uint32_t field, uint64_t value);
aa_proto_status aa_proto_put_bool_field(aa_proto_writer *writer, uint32_t field, bool value);
aa_proto_status aa_proto_put_float_field(aa_proto_writer *writer, uint32_t field, float value);
aa_proto_status aa_proto_put_len_field(aa_proto_writer *writer, uint32_t field,
                                       const uint8_t *data, size_t len);
aa_proto_status aa_proto_put_string_field(aa_proto_writer *writer, uint32_t field,
                                          const char *text);

/* Nested message: begin writes the field key and returns a marker, end writes
 * the length prefix by moving the body inside the same buffer.  The prefix
 * length is therefore not known until end, and a too-small buffer fails at
 * end, not at begin. */
aa_proto_status aa_proto_begin_message(aa_proto_writer *writer, uint32_t field, size_t *marker);
aa_proto_status aa_proto_end_message(aa_proto_writer *writer, size_t marker);

#ifdef __cplusplus
}
#endif

#endif /* AA_PROTO_H */
