/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Android Open Accessory (AOA/AOAP) accessory-mode responder.
 *
 * This is the half of the wired link that runs on the side that *is* the
 * accessory.  For our bridge that is the interesting direction: the car's head
 * unit is the USB host, our device must enumerate as an Android phone in
 * accessory mode, and the AA session then rides the bulk pipe.
 *
 * Role facts (from aasdk commit 046b3b381595509d0939fa84b14a90978f46ff63,
 * read-only; nothing copied):
 *   - wired AA: the head unit is the USB host *and* the TLS client
 *     (SSL_set_connect_state), the phone is the accessory *and* the TLS
 *     server;
 *   - wireless AA: the TLS roles are unchanged; only the pipe changes, the
 *     phone dials the endpoint the head unit announced over RFCOMM.
 * So a bridge is head-unit-shaped toward the phone and phone-shaped toward the
 * car.  This module implements the phone-shaped plumbing only; the AA protocol
 * mirror that talks *to* a head unit is a separate checkpoint.
 *
 * Control requests, as the host sends them (bmRequestType is vendor, recipient
 * device; the direction bit matters):
 *   51 GET_PROTOCOL   IN   answer the AOA protocol version we speak (2)
 *   52 SEND_STRING    OUT  wIndex selects the string, payload is text + NUL:
 *                          0 manufacturer, 1 model, 2 description,
 *                          3 version, 4 URI, 5 serial
 *   53 START          OUT  no payload; the accessory re-enumerates and the AA
 *                          pipe opens.  Requests after START are refused.
 *   54..58                 AOA v2 HID/audio extensions; not implemented here and
 *                          reported as unsupported rather than fatal.
 *
 * Identifiers used by the host to find a started accessory: Google vendor ID
 * 0x18D1, product 0x2D00 (0x2D01 when ADB is also exposed), interface class
 * 0xFF with subclass 0x00 and protocol 0x00, one bulk IN and one bulk OUT
 * endpoint.  Which endpoint address is IN is discovered from the descriptor,
 * not assumed.
 *
 * Local policy, not wire facts: strings are bounded to AA_AOAP_MAX_STRING
 * bytes, a string payload must be NUL-terminated, the protocol query must come
 * before any string, and START requires at least one string and the protocol
 * query.  A refused request asks the USB stack to stall the control endpoint
 * and leaves the state alone so a retrying host is not punished.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_AOAP_H
#define AA_AOAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AA_AOAP_REQ_GET_PROTOCOL 51u
#define AA_AOAP_REQ_SEND_STRING 52u
#define AA_AOAP_REQ_START 53u
#define AA_AOAP_REQ_HID_FIRST 54u
#define AA_AOAP_REQ_HID_LAST 58u

#define AA_AOAP_PROTOCOL_VERSION 2u
#define AA_AOAP_STRING_COUNT 6u
#define AA_AOAP_MAX_STRING 127u

#define AA_AOAP_GOOGLE_VENDOR_ID 0x18D1u
#define AA_AOAP_ACCESSORY_PRODUCT_ID 0x2D00u
#define AA_AOAP_ACCESSORY_ADB_PRODUCT_ID 0x2D01u

#define AA_AOAP_INTERFACE_CLASS 0xFFu
#define AA_AOAP_INTERFACE_SUBCLASS 0x00u
#define AA_AOAP_INTERFACE_PROTOCOL 0x00u
#define AA_AOAP_BULK_IN_ENDPOINT 0x81u
#define AA_AOAP_BULK_OUT_ENDPOINT 0x01u
#define AA_AOAP_MAX_PACKET_HIGH_SPEED 512u

/* String indexes of the string table. */
typedef enum {
    AA_AOAP_STRING_MANUFACTURER = 0,
    AA_AOAP_STRING_MODEL = 1,
    AA_AOAP_STRING_DESCRIPTION = 2,
    AA_AOAP_STRING_VERSION = 3,
    AA_AOAP_STRING_URI = 4,
    AA_AOAP_STRING_SERIAL = 5
} aa_aoap_string_type;

typedef enum {
    AA_AOAP_OK = 0,
    AA_AOAP_UNSUPPORTED = 1,
    AA_AOAP_ERR_ARGUMENT = -1,
    AA_AOAP_ERR_STATE = -2,
    AA_AOAP_ERR_REQUEST = -3,
    AA_AOAP_ERR_DIRECTION = -4,
    AA_AOAP_ERR_INDEX = -5,
    AA_AOAP_ERR_FORMAT = -6,
    AA_AOAP_ERR_LENGTH = -7
} aa_aoap_status;

typedef enum {
    AA_AOAP_IDLE = 0,
    AA_AOAP_STRINGS = 1,
    AA_AOAP_STARTED = 2,
    AA_AOAP_FAILED = 3
} aa_aoap_state;

typedef struct {
    uint8_t bm_request_type;
    uint8_t b_request;
    uint16_t w_value;
    uint16_t w_index;
    const uint8_t *data;
    size_t data_len;
} aa_aoap_request;

typedef struct {
    uint8_t data[2];
    size_t data_len;
    /* Ask the USB stack to stall the control endpoint: refused requests must
     * not look like success to the host. */
    bool stall;
} aa_aoap_reply;

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t in_endpoint;
    uint8_t out_endpoint;
    uint16_t max_packet_size;
} aa_aoap_descriptors;

typedef struct {
    aa_aoap_state state;
    uint8_t strings_mask;
    size_t string_lengths[AA_AOAP_STRING_COUNT];
    char strings[AA_AOAP_STRING_COUNT][AA_AOAP_MAX_STRING + 1u];
    uint32_t requests;
    uint32_t refusals;
} aa_aoap;

void aa_aoap_reset(aa_aoap *aoap);

/* Handle one vendor control request from the host.  The reply is always
 * initialised, so a caller can send it unconditionally. */
aa_aoap_status aa_aoap_on_control(aa_aoap *aoap, const aa_aoap_request *request,
                                  aa_aoap_reply *reply);

/* Descriptor values a platform layer needs to expose the accessory.  adb
 * selects the product ID that also advertises ADB. */
void aa_aoap_descriptors_for(bool adb, aa_aoap_descriptors *out);

/* The stored string, or NULL when the host never sent that index. */
const char *aa_aoap_string(const aa_aoap *aoap, unsigned index);

/* True once START was accepted: the accessory re-enumerates and the AA pipe
 * exists. */
bool aa_aoap_started(const aa_aoap *aoap);

const char *aa_aoap_state_name(aa_aoap_state state);
const char *aa_aoap_status_name(aa_aoap_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_AOAP_H */
