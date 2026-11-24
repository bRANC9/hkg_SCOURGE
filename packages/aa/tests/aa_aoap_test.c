/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the AOA accessory-mode responder: the exact request
 * sequence a head-unit host sends, the stored string table, the START gate and
 * the refusal paths.  No USB hardware is involved.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_aoap.h"

static int g_failures;

static void check(const char *name, bool ok)
{
    if (!ok) {
        g_failures++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

/* bmRequestType bits as the host sets them: vendor, device recipient, plus the
 * direction. */
#define REQ_IN 0xC1u
#define REQ_OUT 0x41u

static aa_aoap_request control_in(uint8_t request)
{
    aa_aoap_request req;
    memset(&req, 0, sizeof req);
    req.bm_request_type = REQ_IN;
    req.b_request = request;
    return req;
}

static aa_aoap_request control_out(uint8_t request, uint16_t index, const char *text)
{
    aa_aoap_request req;
    memset(&req, 0, sizeof req);
    req.bm_request_type = REQ_OUT;
    req.b_request = request;
    req.w_index = index;
    req.data = (const uint8_t *)text;
    req.data_len = text == NULL ? 0u : strlen(text) + 1u;
    return req;
}

static void test_host_sequence(void)
{
    aa_aoap aoap;
    aa_aoap_reply reply;
    aa_aoap_reset(&aoap);

    /* 1. Protocol query. */
    aa_aoap_request req = control_in(AA_AOAP_REQ_GET_PROTOCOL);
    check("protocol ok", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_OK);
    check("protocol value", reply.data_len == 2u && reply.data[0] == 2u && reply.data[1] == 0u);
    check("protocol no stall", !reply.stall);
    check("protocol state", aoap.state == AA_AOAP_STRINGS);
    check("no string yet", aa_aoap_string(&aoap, AA_AOAP_STRING_MODEL) == NULL);

    /* 2. The six identification strings, in the order the reference host sends
     *    them.  Trailing NUL is part of the payload and must be stripped. */
    const char *const texts[AA_AOAP_STRING_COUNT] = {"SCOURGE", "AA bridge", "wireless AA",
                                                     "0.1.0", "", "none"};
    for (uint16_t index = 0u; index < AA_AOAP_STRING_COUNT; index++) {
        req = control_out(AA_AOAP_REQ_SEND_STRING, index, texts[index]);
        check("string ok", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_OK);
        check("string no payload", reply.data_len == 0u && !reply.stall);
    }
    check("strings stored", aoap.strings_mask == 0x3Fu);
    check("string manufacturer", strcmp(aa_aoap_string(&aoap, AA_AOAP_STRING_MANUFACTURER), "SCOURGE") == 0);
    check("string model", strcmp(aa_aoap_string(&aoap, AA_AOAP_STRING_MODEL), "AA bridge") == 0);
    check("string empty uri", strcmp(aa_aoap_string(&aoap, AA_AOAP_STRING_URI), "") == 0);
    check("string lengths", aoap.string_lengths[AA_AOAP_STRING_DESCRIPTION] == 11u);

    /* 3. START: the accessory re-enumerates from here on. */
    req = control_out(AA_AOAP_REQ_START, 0u, NULL);
    check("start ok", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_OK);
    check("start state", aa_aoap_started(&aoap));

    /* After START the handshake is closed. */
    req = control_in(AA_AOAP_REQ_GET_PROTOCOL);
    check("protocol after start", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_ERR_STATE);
    check("protocol after start stalls", reply.stall && aoap.refusals == 1u);
    req = control_out(AA_AOAP_REQ_START, 0u, NULL);
    check("start twice", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_ERR_STATE);

    check("request count", aoap.requests == 10u);
}

static void test_refusals(void)
{
    aa_aoap aoap;
    aa_aoap_reply reply;
    aa_aoap_reset(&aoap);

    /* A string before the protocol query has no state to live in. */
    aa_aoap_request req = control_out(AA_AOAP_REQ_SEND_STRING, AA_AOAP_STRING_MODEL, "x");
    check("string before protocol", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_ERR_STATE);
    check("string before protocol stalls", reply.stall);

    /* Protocol query with the wrong direction. */
    req = control_out(AA_AOAP_REQ_GET_PROTOCOL, 0u, NULL);
    check("protocol wrong direction",
          aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_ERR_DIRECTION);

    /* START before any string. */
    req = control_in(AA_AOAP_REQ_GET_PROTOCOL);
    (void)aa_aoap_on_control(&aoap, &req, &reply);
    req = control_out(AA_AOAP_REQ_START, 0u, NULL);
    check("start without strings", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_ERR_STATE);
    check("start without strings stalls", reply.stall && !aa_aoap_started(&aoap));

    /* Index out of range, missing terminator and oversize payload. */
    req = control_out(AA_AOAP_REQ_SEND_STRING, 6u, "x");
    check("string index", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_ERR_INDEX);

    aa_aoap_request raw;
    memset(&raw, 0, sizeof raw);
    raw.bm_request_type = REQ_OUT;
    raw.b_request = AA_AOAP_REQ_SEND_STRING;
    raw.w_index = AA_AOAP_STRING_MODEL;
    raw.data = (const uint8_t *)"no terminator";
    raw.data_len = 13u;
    check("string unterminated", aa_aoap_on_control(&aoap, &raw, &reply) == AA_AOAP_ERR_FORMAT);

    static uint8_t big[AA_AOAP_MAX_STRING + 2u];
    memset(big, 'a', sizeof big);
    big[sizeof big - 1u] = 0u;
    raw.data = big;
    raw.data_len = sizeof big;
    check("string oversize", aa_aoap_on_control(&aoap, &raw, &reply) == AA_AOAP_ERR_LENGTH);

    /* A payload with no bytes at all is not a string, but a single NUL is a
     * legal empty one (the reference host sends value + NUL unconditionally). */
    raw.data = (const uint8_t *)"\0";
    raw.data_len = 0u;
    check("string empty payload", aa_aoap_on_control(&aoap, &raw, &reply) == AA_AOAP_ERR_FORMAT);
    raw.data = (const uint8_t *)"\0";
    raw.data_len = 1u;
    raw.w_index = AA_AOAP_STRING_SERIAL;
    check("string single nul ok", aa_aoap_on_control(&aoap, &raw, &reply) == AA_AOAP_OK);
    check("string single nul empty", strcmp(aa_aoap_string(&aoap, AA_AOAP_STRING_SERIAL), "") == 0);

    /* AOA v2 HID requests are reported, not fatal, and do not touch the state. */
    req = control_out(AA_AOAP_REQ_HID_FIRST, 0u, NULL);
    check("hid unsupported", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_UNSUPPORTED);
    check("hid keeps state", aoap.state == AA_AOAP_STRINGS);

    /* An unknown vendor request is refused. */
    req = control_out(200u, 0u, NULL);
    check("unknown request", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_ERR_REQUEST);
    check("unknown request stalls", reply.stall);

    /* Argument handling. */
    check("null aoap", aa_aoap_on_control(NULL, &req, &reply) == AA_AOAP_ERR_ARGUMENT);
    check("string out of range", aa_aoap_string(&aoap, 9u) == NULL);

    /* Refusals keep the handshake usable: a valid sequence still completes. */
    req = control_out(AA_AOAP_REQ_SEND_STRING, AA_AOAP_STRING_MANUFACTURER, "SCOURGE");
    check("recovery string", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_OK);
    req = control_out(AA_AOAP_REQ_START, 0u, NULL);
    check("recovery start", aa_aoap_on_control(&aoap, &req, &reply) == AA_AOAP_OK);
    check("recovery started", aa_aoap_started(&aoap));
}

static void test_descriptors_and_names(void)
{
    aa_aoap_descriptors descriptors;
    aa_aoap_descriptors_for(false, &descriptors);
    check("vendor id", descriptors.vendor_id == 0x18D1u);
    check("product id", descriptors.product_id == 0x2D00u);
    check("interface class", descriptors.interface_class == 0xFFu);
    check("interface subclass", descriptors.interface_subclass == 0x00u);
    check("interface protocol", descriptors.interface_protocol == 0x00u);
    check("bulk in", descriptors.in_endpoint == 0x81u && (descriptors.in_endpoint & 0x80u) != 0u);
    check("bulk out", descriptors.out_endpoint == 0x01u && (descriptors.out_endpoint & 0x80u) == 0u);
    check("packet size", descriptors.max_packet_size == 512u);

    aa_aoap_descriptors_for(true, &descriptors);
    check("adb product id", descriptors.product_id == 0x2D01u);

    check("state names", strcmp(aa_aoap_state_name(AA_AOAP_STARTED), "started") == 0);
    check("status names", strcmp(aa_aoap_status_name(AA_AOAP_ERR_DIRECTION), "direction") == 0);
}

int main(void)
{
    test_host_sequence();
    test_refusals();
    test_descriptors_and_names();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_aoap: all checks passed\n");
    return 0;
}
