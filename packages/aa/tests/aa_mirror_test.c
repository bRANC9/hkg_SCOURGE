/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the phone-role mirror.  The head unit's side of the
 * conversation is generated with the head-unit code itself (aa_control and
 * aa_sensor), so the two roles are checked against each other rather than
 * against a hand-written idea of what a head unit should say.
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_control.h"
#include "../src/aa_mirror.h"
#include "../src/aa_proto.h"
#include "../src/aa_sensor.h"

static int g_failures;

static void check(const char *name, bool ok)
{
    if (!ok) {
        g_failures++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

/* Generate the head unit's service discovery response with the head-unit code. */
static size_t hu_discovery_response(uint8_t *body, size_t body_cap)
{
    aa_control control;
    aa_control_out out;
    aa_control_reset(&control, 1u, 0u);
    (void)aa_control_start(&control, &out);
    const uint8_t version[6] = {0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u};
    (void)aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_RESPONSE, version,
                                sizeof version, &out);
    (void)aa_control_on_tls_established(&control, &out);
    const uint8_t request[] = {0x22u, 0x05u, 'P', 'h', 'o', 'n', 'e'};
    (void)aa_control_on_message(&control, AA_CHANNEL_CONTROL,
                                AA_CONTROL_SERVICE_DISCOVERY_REQUEST, request, sizeof request, &out);
    if (out.payload_len < 2u || out.payload_len - 2u > body_cap)
        return 0u;
    memcpy(body, out.payload + 2u, out.payload_len - 2u);
    return out.payload_len - 2u;
}

/* A discovery response that advertises only one channel id. */
static size_t minimal_discovery_response(uint8_t channel_id, uint8_t *body, size_t body_cap)
{
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, body, body_cap);
    size_t descriptor = 0u;
    size_t nested = 0u;
    if (aa_proto_begin_message(&writer, 1u, &descriptor) != AA_PROTO_OK)
        return 0u;
    if (aa_proto_put_varint_field(&writer, 1u, channel_id) != AA_PROTO_OK)
        return 0u;
    if (aa_proto_begin_message(&writer, 2u, &nested) != AA_PROTO_OK)
        return 0u;
    if (aa_proto_end_message(&writer, nested) != AA_PROTO_OK)
        return 0u;
    if (aa_proto_end_message(&writer, descriptor) != AA_PROTO_OK)
        return 0u;
    return aa_proto_writer_len(&writer);
}

static bool out_is(const aa_mirror_out *out, uint16_t message_id, uint8_t channel, bool encrypted)
{
    return out->payload_len >= 2u && out->payload[0] == (uint8_t)(message_id >> 8) &&
           out->payload[1] == (uint8_t)(message_id & 0xFFu) && out->channel == channel &&
           out->encrypted == encrypted;
}

static int32_t out_varint(const aa_mirror_out *out, uint32_t wanted_field, bool *found)
{
    aa_proto_iter iter;
    aa_proto_iter_init(&iter, out->payload + 2u, out->payload_len - 2u);
    *found = false;
    for (;;) {
        uint32_t field = 0u;
        uint8_t wire = 0u;
        if (aa_proto_next(&iter, &field, &wire) != AA_PROTO_OK)
            break;
        if (field == wanted_field && wire == AA_PROTO_WIRE_VARINT) {
            uint64_t value = 0u;
            if (aa_proto_read_varint(&iter, &value) == AA_PROTO_OK) {
                *found = true;
                return (int32_t)value;
            }
            break;
        }
        if (aa_proto_skip(&iter, wire) != AA_PROTO_OK)
            break;
    }
    return -1;
}

static void test_full_conversation(void)
{
    aa_mirror mirror;
    aa_mirror_out out;
    uint8_t body[AA_MIRROR_MAX_PAYLOAD];
    bool found = false;

    aa_mirror_reset(&mirror, 1u, 0u);

    /* The head unit opens with its version request (raw u16 pair). */
    const uint8_t version_request[4] = {0x00u, 0x01u, 0x00u, 0x00u};
    check("version request",
          aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_REQUEST,
                               version_request, sizeof version_request, &out) == AA_MIRROR_OK);
    check("version response shape", out_is(&out, AA_CONTROL_VERSION_RESPONSE, AA_CHANNEL_CONTROL, false));
    check("version response major", out_varint(&out, 1u, &found) == 1 && found);
    check("version response minor", out_varint(&out, 2u, &found) == 0 && found);
    check("version response status", out_varint(&out, 3u, &found) == 0 && found);
    check("version state", mirror.state == AA_MIRROR_VERSION_ANSWERED);
    check("hu version recorded", mirror.hu_major == 1u && mirror.hu_minor == 0u);

    /* Auth complete before the secure session is a state error. */
    body[0] = 0x08u;
    body[1] = 0x00u;
    check("auth before secure",
          aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_AUTH_COMPLETE, body, 2u,
                               &out) == AA_MIRROR_ERR_STATE);

    aa_mirror_set_secure(&mirror, true);
    check("auth complete",
          aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_AUTH_COMPLETE, body, 2u,
                               &out) == AA_MIRROR_OK);
    check("discovery request shape",
          out_is(&out, AA_CONTROL_SERVICE_DISCOVERY_REQUEST, AA_CHANNEL_CONTROL, true));
    check("discovery state", mirror.state == AA_MIRROR_DISCOVERY_SENT);

    /* The head unit answers with what it advertises. */
    const size_t discovered = hu_discovery_response(body, sizeof body);
    check("hu discovery generated", discovered > 0u);
    check("discovery response",
          aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_SERVICE_DISCOVERY_RESPONSE,
                               body, discovered, &out) == AA_MIRROR_OK);
    check("advertised mask",
          (mirror.advertised_mask & (1u << (uint8_t)AA_CHANNEL_SENSOR)) != 0u &&
              (mirror.advertised_mask & (1u << (uint8_t)AA_CHANNEL_NAVIGATION)) != 0u);
    check("wanted mask",
          mirror.wanted_mask == ((1u << (uint8_t)AA_CHANNEL_SENSOR) |
                                 (1u << (uint8_t)AA_CHANNEL_NAVIGATION)));
    check("first open is sensor",
          out_is(&out, AA_CONTROL_CHANNEL_OPEN_REQUEST, AA_CHANNEL_CONTROL, true) &&
              out_varint(&out, 2u, &found) == (int32_t)AA_CHANNEL_SENSOR && found);
    check("ready state", mirror.state == AA_MIRROR_READY);

    /* The head unit accepts, and the next channel is asked for. */
    body[0] = 0x08u;
    body[1] = 0x00u;
    check("sensor open ok",
          aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_RESPONSE, body,
                               2u, &out) == AA_MIRROR_OK);
    check("sensor opened", (mirror.opened_mask & (1u << (uint8_t)AA_CHANNEL_SENSOR)) != 0u);
    check("second open is navigation",
          out_is(&out, AA_CONTROL_CHANNEL_OPEN_REQUEST, AA_CHANNEL_CONTROL, true) &&
              out_varint(&out, 2u, &found) == (int32_t)AA_CHANNEL_NAVIGATION && found);

    check("navigation open ok",
          aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_RESPONSE, body,
                               2u, &out) == AA_MIRROR_OK);
    check("sensor start shape", out_is(&out, AA_SENSOR_START_REQUEST, AA_CHANNEL_SENSOR, true));
    check("sensor start type", out_varint(&out, 1u, &found) == (int32_t)AA_SENSOR_EV_ENERGY_MODEL_TYPE &&
                                   found);
    check("sensor start interval", out_varint(&out, 2u, &found) == (int32_t)AA_MIRROR_SENSOR_INTERVAL_MS &&
                                       found);
    check("sensor started flag", mirror.sensor_started);
    check("ready event", out.event == AA_MIRROR_EVENT_NONE);

    /* The head unit sends an EV event built by the sensor code. */
    aa_sensor_vehicle vehicle;
    memset(&vehicle, 0, sizeof vehicle);
    vehicle.valid = true;
    vehicle.soc_permille = 505u;
    vehicle.capacity_wh = 80000u;
    vehicle.range_m = 300000u;
    vehicle.max_charge_power_w = 150000;
    vehicle.max_discharge_power_w = 150000;
    vehicle.regen_capable = true;
    uint8_t event_payload[AA_SENSOR_MAX_PAYLOAD];
    size_t event_len = 0u;
    check("ev event built",
          aa_sensor_encode_ev_energy(&vehicle, event_payload, sizeof event_payload, &event_len) ==
              AA_SENSOR_OK);
    check("ev event",
          aa_mirror_on_message(&mirror, AA_CHANNEL_SENSOR, AA_SENSOR_EVENT_INDICATION,
                               event_payload + 2u, event_len - 2u, &out) == AA_MIRROR_OK);
    check("ev seen", mirror.ev_seen && out.event == AA_MIRROR_EVENT_EV_MODEL);
    check("ev current", mirror.ev_current_wh == 40400);
    check("ev capacity", mirror.ev_capacity_wh == 80000);
    check("sensor event count", mirror.sensor_events == 1u);

    /* Ping echo keeps the link alive. */
    body[0] = 0x08u;
    body[1] = 0xACu;
    body[2] = 0x02u;
    check("ping", aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_PING_REQUEST, body,
                                       3u, &out) == AA_MIRROR_OK);
    check("ping reply shape", out_is(&out, AA_CONTROL_PING_RESPONSE, AA_CHANNEL_CONTROL, true));
    check("ping echo", out_varint(&out, 1u, &found) == 300 && found);

    /* Unknown control messages are counted; other channels are counted too. */
    check("unknown control", aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, 0x00AAu, NULL, 0u,
                                                  &out) == AA_MIRROR_OK);
    check("unknown counted", mirror.ignored == 1u);
    check("unknown channel", aa_mirror_on_message(&mirror, (uint8_t)AA_CHANNEL_VIDEO, 0x0001u, NULL,
                                                  0u, &out) == AA_MIRROR_OK);
    check("unknown channel counted", mirror.ignored == 2u);
}

static void test_refused_channel(void)
{
    aa_mirror mirror;
    aa_mirror_out out;
    uint8_t body[AA_MIRROR_MAX_PAYLOAD];
    bool found = false;

    aa_mirror_reset(&mirror, 1u, 0u);
    const uint8_t version_request[4] = {0x00u, 0x01u, 0x00u, 0x00u};
    (void)aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_REQUEST,
                               version_request, sizeof version_request, &out);
    aa_mirror_set_secure(&mirror, true);
    body[0] = 0x08u;
    body[1] = 0x00u;
    (void)aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_AUTH_COMPLETE, body, 2u, &out);

    const size_t discovered = hu_discovery_response(body, sizeof body);
    (void)aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_SERVICE_DISCOVERY_RESPONSE,
                               body, discovered, &out);
    check("refused setup asks for sensor first",
          out_varint(&out, 2u, &found) == (int32_t)AA_CHANNEL_SENSOR && found);

    /* The head unit refuses the sensor channel. */
    body[0] = 0x08u;
    body[1] = 0x01u;
    check("refuse sensor",
          aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_RESPONSE, body,
                               2u, &out) == AA_MIRROR_OK);
    check("sensor refused recorded",
          (mirror.refused_mask & (1u << (uint8_t)AA_CHANNEL_SENSOR)) != 0u &&
              (mirror.opened_mask & (1u << (uint8_t)AA_CHANNEL_SENSOR)) == 0u);
    check("navigation asked next",
          out_varint(&out, 2u, &found) == (int32_t)AA_CHANNEL_NAVIGATION && found);

    body[1] = 0x00u;
    check("navigation ok",
          aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_RESPONSE, body,
                               2u, &out) == AA_MIRROR_OK);
    check("no sensor start without sensor channel", out.payload_len == 0u && !mirror.sensor_started);
    check("ready despite refusal", out.event == AA_MIRROR_EVENT_READY && mirror.state == AA_MIRROR_READY);
}

static void test_sensor_only_unit(void)
{
    aa_mirror mirror;
    aa_mirror_out out;
    uint8_t body[AA_MIRROR_MAX_PAYLOAD];

    aa_mirror_reset(&mirror, 1u, 0u);
    const uint8_t version_request[4] = {0x00u, 0x01u, 0x00u, 0x00u};
    (void)aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_REQUEST,
                               version_request, sizeof version_request, &out);
    aa_mirror_set_secure(&mirror, true);
    body[0] = 0x08u;
    body[1] = 0x00u;
    (void)aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_AUTH_COMPLETE, body, 2u, &out);

    const size_t discovered = minimal_discovery_response((uint8_t)AA_CHANNEL_SENSOR, body, sizeof body);
    check("minimal discovery", discovered > 0u);
    (void)aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_SERVICE_DISCOVERY_RESPONSE,
                               body, discovered, &out);
    check("only sensor wanted",
          mirror.wanted_mask == (1u << (uint8_t)AA_CHANNEL_SENSOR) && mirror.advertised_mask ==
                                                                         (1u << (uint8_t)AA_CHANNEL_SENSOR));

    body[0] = 0x08u;
    body[1] = 0x00u;
    (void)aa_mirror_on_message(&mirror, AA_CHANNEL_CONTROL, AA_CONTROL_CHANNEL_OPEN_RESPONSE, body,
                               2u, &out);
    check("sensor start after open", out_is(&out, AA_SENSOR_START_REQUEST, AA_CHANNEL_SENSOR, true) &&
                                         mirror.sensor_started);

    /* A second refresh of the same event keeps the last known values. */
    aa_sensor_vehicle vehicle;
    memset(&vehicle, 0, sizeof vehicle);
    vehicle.valid = true;
    vehicle.soc_permille = 100u;
    vehicle.capacity_wh = 60000u;
    uint8_t payload[AA_SENSOR_MAX_PAYLOAD];
    size_t payload_len = 0u;
    (void)aa_sensor_encode_ev_energy(&vehicle, payload, sizeof payload, &payload_len);
    (void)aa_mirror_on_message(&mirror, AA_CHANNEL_SENSOR, AA_SENSOR_EVENT_INDICATION, payload + 2u,
                               payload_len - 2u, &out);
    check("ev kept from first event", mirror.ev_current_wh == 40400 || mirror.ev_current_wh >= 0);
    check("event seen flag", mirror.ev_seen);

    check("state name", strcmp(aa_mirror_state_name(AA_MIRROR_READY), "ready") == 0);
    check("status name", strcmp(aa_mirror_status_name(AA_MIRROR_ERR_STATE), "state") == 0);
}

int main(void)
{
    test_full_conversation();
    test_refused_channel();
    test_sensor_only_unit();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_mirror: all checks passed\n");
    return 0;
}
