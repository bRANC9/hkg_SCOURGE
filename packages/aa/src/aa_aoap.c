/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * AOA accessory-mode responder implementation.  See aa_aoap.h for the request
 * table, the role facts and the local policy.
 */
#include "aa_aoap.h"

#include <string.h>

#define AA_AOAP_DIRECTION_IN 0x80u

static bool direction_in(const aa_aoap_request *request)
{
    return (request->bm_request_type & AA_AOAP_DIRECTION_IN) != 0u;
}

static aa_aoap_status refuse(aa_aoap *aoap, aa_aoap_reply *reply, aa_aoap_status status)
{
    reply->data_len = 0u;
    reply->stall = true;
    aoap->refusals++;
    return status;
}

void aa_aoap_reset(aa_aoap *aoap)
{
    if (aoap == NULL)
        return;
    memset(aoap, 0, sizeof *aoap);
    aoap->state = AA_AOAP_IDLE;
}

aa_aoap_status aa_aoap_on_control(aa_aoap *aoap, const aa_aoap_request *request,
                                  aa_aoap_reply *reply)
{
    if (aoap == NULL || request == NULL || reply == NULL)
        return AA_AOAP_ERR_ARGUMENT;

    reply->data_len = 0u;
    reply->stall = false;
    aoap->requests++;

    switch (request->b_request) {
    case AA_AOAP_REQ_GET_PROTOCOL:
        if (!direction_in(request))
            return refuse(aoap, reply, AA_AOAP_ERR_DIRECTION);
        if (aoap->state == AA_AOAP_STARTED)
            return refuse(aoap, reply, AA_AOAP_ERR_STATE);
        reply->data[0] = (uint8_t)(AA_AOAP_PROTOCOL_VERSION & 0xFFu);
        reply->data[1] = (uint8_t)(AA_AOAP_PROTOCOL_VERSION >> 8);
        reply->data_len = 2u;
        if (aoap->state == AA_AOAP_IDLE)
            aoap->state = AA_AOAP_STRINGS;
        return AA_AOAP_OK;

    case AA_AOAP_REQ_SEND_STRING: {
        if (direction_in(request))
            return refuse(aoap, reply, AA_AOAP_ERR_DIRECTION);
        if (request->w_index >= AA_AOAP_STRING_COUNT)
            return refuse(aoap, reply, AA_AOAP_ERR_INDEX);
        /* One NUL is a legal empty string; a payload without a terminator is
         * not, and neither is an address with no bytes at all. */
        if (request->data == NULL || request->data_len < 1u)
            return refuse(aoap, reply, AA_AOAP_ERR_FORMAT);
        if (request->data_len > AA_AOAP_MAX_STRING + 1u)
            return refuse(aoap, reply, AA_AOAP_ERR_LENGTH);
        if (request->data[request->data_len - 1u] != 0u)
            return refuse(aoap, reply, AA_AOAP_ERR_FORMAT);
        if (aoap->state != AA_AOAP_STRINGS)
            return refuse(aoap, reply, AA_AOAP_ERR_STATE);

        const size_t text_len = request->data_len - 1u;
        memcpy(aoap->strings[request->w_index], request->data, text_len);
        aoap->strings[request->w_index][text_len] = '\0';
        aoap->string_lengths[request->w_index] = text_len;
        aoap->strings_mask = (uint8_t)(aoap->strings_mask | (1u << request->w_index));
        return AA_AOAP_OK;
    }

    case AA_AOAP_REQ_START:
        if (direction_in(request))
            return refuse(aoap, reply, AA_AOAP_ERR_DIRECTION);
        if (aoap->state != AA_AOAP_STRINGS)
            return refuse(aoap, reply, AA_AOAP_ERR_STATE);
        if (aoap->strings_mask == 0u)
            return refuse(aoap, reply, AA_AOAP_ERR_STATE);
        aoap->state = AA_AOAP_STARTED;
        return AA_AOAP_OK;

    default:
        /* AOA v2 HID/audio requests are not implemented, but their absence is
         * not a handshake failure: report them and let the host decide. */
        if (request->b_request >= AA_AOAP_REQ_HID_FIRST &&
            request->b_request <= AA_AOAP_REQ_HID_LAST) {
            reply->stall = true;
            return AA_AOAP_UNSUPPORTED;
        }
        return refuse(aoap, reply, AA_AOAP_ERR_REQUEST);
    }
}

void aa_aoap_descriptors_for(bool adb, aa_aoap_descriptors *out)
{
    if (out == NULL)
        return;
    out->vendor_id = AA_AOAP_GOOGLE_VENDOR_ID;
    out->product_id = adb ? AA_AOAP_ACCESSORY_ADB_PRODUCT_ID : AA_AOAP_ACCESSORY_PRODUCT_ID;
    out->interface_class = AA_AOAP_INTERFACE_CLASS;
    out->interface_subclass = AA_AOAP_INTERFACE_SUBCLASS;
    out->interface_protocol = AA_AOAP_INTERFACE_PROTOCOL;
    out->in_endpoint = AA_AOAP_BULK_IN_ENDPOINT;
    out->out_endpoint = AA_AOAP_BULK_OUT_ENDPOINT;
    out->max_packet_size = AA_AOAP_MAX_PACKET_HIGH_SPEED;
}

const char *aa_aoap_string(const aa_aoap *aoap, unsigned index)
{
    if (aoap == NULL || index >= AA_AOAP_STRING_COUNT)
        return NULL;
    if ((aoap->strings_mask & (1u << index)) == 0u)
        return NULL;
    return aoap->strings[index];
}

bool aa_aoap_started(const aa_aoap *aoap)
{
    return aoap != NULL && aoap->state == AA_AOAP_STARTED;
}

const char *aa_aoap_state_name(aa_aoap_state state)
{
    switch (state) {
    case AA_AOAP_IDLE:
        return "idle";
    case AA_AOAP_STRINGS:
        return "strings";
    case AA_AOAP_STARTED:
        return "started";
    case AA_AOAP_FAILED:
        return "failed";
    }
    return "unknown";
}

const char *aa_aoap_status_name(aa_aoap_status status)
{
    switch (status) {
    case AA_AOAP_OK:
        return "ok";
    case AA_AOAP_UNSUPPORTED:
        return "unsupported";
    case AA_AOAP_ERR_ARGUMENT:
        return "argument";
    case AA_AOAP_ERR_STATE:
        return "state";
    case AA_AOAP_ERR_REQUEST:
        return "request";
    case AA_AOAP_ERR_DIRECTION:
        return "direction";
    case AA_AOAP_ERR_INDEX:
        return "index";
    case AA_AOAP_ERR_FORMAT:
        return "format";
    case AA_AOAP_ERR_LENGTH:
        return "length";
    }
    return "unknown";
}
