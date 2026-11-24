/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the protobuf wire-format codec: fixed varint vectors,
 * field iteration, skipping, UTF-8 and string bounds, and the nested-message
 * writer including its failure mode.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_proto.h"

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

static void check_varint_roundtrip(const char *name, uint64_t value, const char *vector)
{
    uint8_t expected[16];
    const size_t expected_len = hex_to_bytes(vector, expected, sizeof expected);
    uint8_t buf[16];
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, buf, sizeof buf);
    check(name, aa_proto_put_varint_field(&writer, 1u, value) == AA_PROTO_OK);
    check(name, aa_proto_writer_len(&writer) == expected_len + 1u);

    /* key 0x08, then the vector */
    check(name, buf[0] == 0x08u && memcmp(buf + 1, expected, expected_len) == 0);

    aa_proto_iter iter;
    aa_proto_iter_init(&iter, buf, aa_proto_writer_len(&writer));
    uint32_t field = 0u;
    uint8_t wire = 0u;
    check(name, aa_proto_next(&iter, &field, &wire) == AA_PROTO_OK);
    check(name, field == 1u && wire == AA_PROTO_WIRE_VARINT);
    uint64_t decoded = 0u;
    check(name, aa_proto_read_varint(&iter, &decoded) == AA_PROTO_OK);
    check(name, decoded == value);
    check(name, aa_proto_next(&iter, &field, &wire) == AA_PROTO_END);
}

static void test_varints(void)
{
    check_varint_roundtrip("varint 0", 0u, "00");
    check_varint_roundtrip("varint 1", 1u, "01");
    check_varint_roundtrip("varint 127", 127u, "7f");
    check_varint_roundtrip("varint 128", 128u, "8001");
    check_varint_roundtrip("varint 300", 300u, "ac02");
    check_varint_roundtrip("varint 16384", 16384u, "808001");
}

static void test_varint_errors(void)
{
    uint8_t buf[16];
    aa_proto_iter iter;
    uint64_t value = 0u;

    buf[0] = 0x80u;
    aa_proto_iter_init(&iter, buf, 1u);
    check("truncated varint", aa_proto_read_varint(&iter, &value) == AA_PROTO_ERR_TRUNCATED);

    for (size_t i = 0u; i < 10u; i++)
        buf[i] = 0x80u;
    aa_proto_iter_init(&iter, buf, 10u);
    check("eleven byte varint", aa_proto_read_varint(&iter, &value) == AA_PROTO_ERR_VARINT);

    for (size_t i = 0u; i < 9u; i++)
        buf[i] = 0x80u;
    buf[9] = 0x02u; /* tenth byte carries more than the last bit */
    aa_proto_iter_init(&iter, buf, 10u);
    check("overflow varint", aa_proto_read_varint(&iter, &value) == AA_PROTO_ERR_VARINT);

    /* Field 0 and a group wire type are both rejected. */
    buf[0] = 0x00u;
    aa_proto_iter_init(&iter, buf, 1u);
    uint32_t field = 0u;
    uint8_t wire = 0u;
    check("field zero", aa_proto_next(&iter, &field, &wire) == AA_PROTO_ERR_WIRE);
    buf[0] = 0x0Bu; /* field 1, wire type 3 (group) */
    aa_proto_iter_init(&iter, buf, 1u);
    check("group wire", aa_proto_next(&iter, &field, &wire) == AA_PROTO_ERR_WIRE);
}

static void test_field_iteration(void)
{
    /* field 1 varint 5, field 2 bytes "ab", field 3 fixed32, field 4 fixed64,
     * field 9 varint 1 (unknown to the caller, skipped). */
    uint8_t buf[32];
    size_t len = hex_to_bytes("0805120261621d000000002100000000000000004801", buf, sizeof buf);
    check("iteration len", len == 22u);

    aa_proto_iter iter;
    aa_proto_iter_init(&iter, buf, len);
    uint32_t field = 0u;
    uint8_t wire = 0u;
    uint64_t value = 0u;

    check("iteration field 1", aa_proto_next(&iter, &field, &wire) == AA_PROTO_OK);
    check("iteration field 1 ok", field == 1u && wire == AA_PROTO_WIRE_VARINT);
    check("iteration field 1 value", aa_proto_read_varint(&iter, &value) == AA_PROTO_OK && value == 5u);

    check("iteration field 2", aa_proto_next(&iter, &field, &wire) == AA_PROTO_OK);
    check("iteration field 2 ok", field == 2u && wire == AA_PROTO_WIRE_LEN);
    const uint8_t *data = NULL;
    size_t data_len = 0u;
    check("iteration field 2 value",
          aa_proto_read_len(&iter, &data, &data_len) == AA_PROTO_OK && data_len == 2u &&
              memcmp(data, "ab", 2) == 0);

    check("iteration field 3", aa_proto_next(&iter, &field, &wire) == AA_PROTO_OK);
    check("iteration field 3 skip", aa_proto_skip(&iter, wire) == AA_PROTO_OK);

    check("iteration field 4", aa_proto_next(&iter, &field, &wire) == AA_PROTO_OK);
    check("iteration field 4 skip", aa_proto_skip(&iter, wire) == AA_PROTO_OK);

    check("iteration field 9", aa_proto_next(&iter, &field, &wire) == AA_PROTO_OK);
    check("iteration field 9 ok", field == 9u && wire == AA_PROTO_WIRE_VARINT);
    check("iteration field 9 skip", aa_proto_skip(&iter, wire) == AA_PROTO_OK);

    check("iteration end", aa_proto_next(&iter, &field, &wire) == AA_PROTO_END);
}

static void test_strings(void)
{
    uint8_t buf[64];
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, buf, sizeof buf);
    check("string write", aa_proto_put_string_field(&writer, 4u, "Phone") == AA_PROTO_OK);
    check("string write len", aa_proto_writer_len(&writer) == 7u);
    check("string write key", buf[0] == 0x22u && buf[1] == 0x05u);

    aa_proto_iter iter;
    aa_proto_iter_init(&iter, buf, aa_proto_writer_len(&writer));
    uint32_t field = 0u;
    uint8_t wire = 0u;
    check("string read field", aa_proto_next(&iter, &field, &wire) == AA_PROTO_OK);
    char text[16];
    size_t text_len = 0u;
    check("string read",
          aa_proto_read_string(&iter, text, sizeof text, &text_len) == AA_PROTO_OK &&
              text_len == 5u && strcmp(text, "Phone") == 0);

    /* A buffer without room for the terminator is a hard error. */
    aa_proto_iter_init(&iter, buf, aa_proto_writer_len(&writer));
    check("string read field 2", aa_proto_next(&iter, &field, &wire) == AA_PROTO_OK);
    check("string too small",
          aa_proto_read_string(&iter, text, 5u, &text_len) == AA_PROTO_ERR_OVERFLOW);

    /* Invalid UTF-8 and an embedded NUL are rejected. */
    check("utf8 valid", aa_proto_utf8_valid("Phone", 5u));
    check("utf8 two byte", aa_proto_utf8_valid("\xC3\xA9", 2u));
    check("utf8 bad continuation", !aa_proto_utf8_valid("\xC3\x28", 2u));
    check("utf8 surrogate", !aa_proto_utf8_valid("\xED\xA0\x80", 3u));
    check("utf8 embedded nul", !aa_proto_utf8_valid("a\0b", 3u));
}

static void test_nested_writer(void)
{
    uint8_t buf[32];
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, buf, sizeof buf);

    size_t marker = 0u;
    check("nested begin", aa_proto_begin_message(&writer, 1u, &marker) == AA_PROTO_OK);
    check("nested body", aa_proto_put_varint_field(&writer, 1u, 300u) == AA_PROTO_OK);
    check("nested end", aa_proto_end_message(&writer, marker) == AA_PROTO_OK);

    uint8_t expected[8];
    const size_t expected_len = hex_to_bytes("0a0308ac02", expected, sizeof expected);
    check("nested vector len", aa_proto_writer_len(&writer) == expected_len);
    check("nested vector bytes", memcmp(buf, expected, expected_len) == 0);

    /* Two nested messages in one buffer keep their own lengths. */
    aa_proto_writer_init(&writer, buf, sizeof buf);
    check("nested2 begin a", aa_proto_begin_message(&writer, 1u, &marker) == AA_PROTO_OK);
    check("nested2 body a", aa_proto_put_varint_field(&writer, 1u, 2u) == AA_PROTO_OK);
    check("nested2 end a", aa_proto_end_message(&writer, marker) == AA_PROTO_OK);
    check("nested2 begin b", aa_proto_begin_message(&writer, 1u, &marker) == AA_PROTO_OK);
    check("nested2 body b", aa_proto_put_varint_field(&writer, 1u, 9u) == AA_PROTO_OK);
    check("nested2 end b", aa_proto_end_message(&writer, marker) == AA_PROTO_OK);
    {
        uint8_t two[8];
        const size_t two_len = hex_to_bytes("0a0208020a020809", two, sizeof two);
        check("nested2 bytes",
              aa_proto_writer_len(&writer) == two_len && memcmp(buf, two, two_len) == 0);
    }

    /* Writer overflow poisons the writer. */
    uint8_t tiny[3];
    aa_proto_writer_init(&writer, tiny, sizeof tiny);
    check("tiny overflow", aa_proto_put_varint_field(&writer, 1u, 16384u) == AA_PROTO_ERR_OVERFLOW);
    check("tiny poisoned", !aa_proto_writer_ok(&writer));
    check("tiny stays failed", aa_proto_put_bool_field(&writer, 2u, true) == AA_PROTO_ERR_OVERFLOW);

    /* A marker beyond the current length is an argument error. */
    aa_proto_writer_init(&writer, tiny, sizeof tiny);
    check("bad marker", aa_proto_end_message(&writer, 4u) == AA_PROTO_ERR_ARGUMENT);
}

static void test_nested_end_overflow(void)
{
    /* Field key plus a one-byte body fits, but the length prefix does not. */
    uint8_t buf[3];
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, buf, sizeof buf);
    size_t marker = 0u;
    check("tight begin", aa_proto_begin_message(&writer, 1u, &marker) == AA_PROTO_OK);
    check("tight body", aa_proto_put_varint_field(&writer, 1u, 1u) == AA_PROTO_OK);
    check("tight end overflow", aa_proto_end_message(&writer, marker) == AA_PROTO_ERR_OVERFLOW);
    check("tight poisoned", !aa_proto_writer_ok(&writer));
}

int main(void)
{
    test_varints();
    test_varint_errors();
    test_field_iteration();
    test_strings();
    test_nested_writer();
    test_nested_end_overflow();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_proto: all checks passed\n");
    return 0;
}
