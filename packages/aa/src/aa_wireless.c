/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Wireless path orchestration.  See aa_wireless.h for the order and the policy.
 */
#include "aa_wireless.h"

#include <string.h>

void aa_wireless_reset(aa_wireless *wireless)
{
    if (wireless == NULL)
        return;
    memset(wireless, 0, sizeof *wireless);
    aa_btboot_reset(&wireless->bootstrap);
    aa_session_reset(&wireless->session, NULL, 1u, 0u);
}

aa_wireless_status aa_wireless_begin(aa_wireless *wireless, const aa_ap_readback *ap,
                                     const aa_tls_provider *provider, uint8_t *packet,
                                     size_t packet_cap, size_t *packet_len)
{
    if (wireless == NULL || ap == NULL || provider == NULL || packet == NULL ||
        packet_len == NULL)
        return AA_WIRELESS_ERR_ARGUMENT;
    *packet_len = 0u;
    if (wireless->began)
        return AA_WIRELESS_ERR_STATE;

    if (aa_ap_from_readback(ap, &wireless->endpoint) != AA_AP_OK)
        return AA_WIRELESS_ERR_AP;

    aa_btboot_info info;
    aa_btboot_start start;
    if (aa_ap_btboot_info(&wireless->endpoint, &info) != AA_AP_OK ||
        aa_ap_btboot_start(&wireless->endpoint, &start) != AA_AP_OK)
        return AA_WIRELESS_ERR_AP;

    if (aa_btboot_set_credentials(&wireless->bootstrap, &info) != AA_BTBOOT_OK)
        return AA_WIRELESS_ERR_BOOTSTRAP;
    if (aa_btboot_begin(&wireless->bootstrap, &start, packet, packet_cap, packet_len) !=
        AA_BTBOOT_OK)
        return AA_WIRELESS_ERR_BOOTSTRAP;

    aa_session_reset(&wireless->session, provider, 1u, 0u);
    wireless->began = true;
    return AA_WIRELESS_OK;
}

aa_wireless_status aa_wireless_on_bt_packet(aa_wireless *wireless, const uint8_t *data, size_t len,
                                            uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    if (wireless == NULL || data == NULL || reply == NULL || reply_len == NULL)
        return AA_WIRELESS_ERR_ARGUMENT;
    *reply_len = 0u;
    if (!wireless->began)
        return AA_WIRELESS_ERR_STATE;

    uint16_t message_id = 0u;
    const uint8_t *body = NULL;
    size_t body_len = 0u;
    size_t consumed = 0u;
    if (aa_btboot_unpack(data, len, wireless->bootstrap.length_includes_message_id, &message_id,
                         &body, &body_len, &consumed) != AA_BTBOOT_OK)
        return AA_WIRELESS_ERR_FRAMING;
    if (consumed != len)
        return AA_WIRELESS_ERR_FRAMING;

    const aa_btboot_status boot_status = aa_btboot_on_packet(&wireless->bootstrap, message_id, body,
                                                             body_len, reply, reply_cap, reply_len);
    if (boot_status == AA_BTBOOT_ERR_REFUSED || boot_status == AA_BTBOOT_ERR_STATE)
        return AA_WIRELESS_ERR_BOOTSTRAP;
    if (boot_status != AA_BTBOOT_OK)
        return AA_WIRELESS_ERR_BOOTSTRAP;

    if (wireless->bootstrap.state == AA_BTBOOT_CONNECTED)
        wireless->phone_connected = true;
    return AA_WIRELESS_OK;
}

bool aa_wireless_ready_for_tcp(const aa_wireless *wireless)
{
    return wireless != NULL && wireless->phone_connected;
}

aa_wireless_status aa_wireless_start_session(aa_wireless *wireless, aa_session_out *out)
{
    if (wireless == NULL || out == NULL)
        return AA_WIRELESS_ERR_ARGUMENT;
    if (!wireless->phone_connected || wireless->session_started)
        return AA_WIRELESS_ERR_STATE;
    if (aa_session_start(&wireless->session, out) != AA_SESSION_OK)
        return AA_WIRELESS_ERR_SESSION;
    wireless->session_started = true;
    return AA_WIRELESS_OK;
}

aa_session_status aa_wireless_on_tcp(aa_wireless *wireless, const uint8_t *data, size_t len,
                                     aa_session_out *out)
{
    if (wireless == NULL || out == NULL)
        return AA_SESSION_ERR_ARGUMENT;
    if (!wireless->phone_connected || !wireless->session_started)
        return AA_SESSION_ERR_STATE;
    return aa_session_feed(&wireless->session, data, len, out);
}

aa_session *aa_wireless_session(aa_wireless *wireless)
{
    return wireless == NULL ? NULL : &wireless->session;
}

const char *aa_wireless_status_name(aa_wireless_status status)
{
    switch (status) {
    case AA_WIRELESS_OK:
        return "ok";
    case AA_WIRELESS_ERR_ARGUMENT:
        return "argument";
    case AA_WIRELESS_ERR_STATE:
        return "state";
    case AA_WIRELESS_ERR_AP:
        return "ap";
    case AA_WIRELESS_ERR_BOOTSTRAP:
        return "bootstrap";
    case AA_WIRELESS_ERR_FRAMING:
        return "framing";
    case AA_WIRELESS_ERR_SESSION:
        return "session";
    }
    return "unknown";
}
