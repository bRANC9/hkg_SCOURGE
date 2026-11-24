/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Helper for the differential checks: prints the encoder output the C codec
 * really produces, so the official protobuf tooling can decode it, and parses
 * an officially encoded message so our decoder can be compared against it.
 * This is a test tool, not part of the library.
 *
 * Modes:
 *   discovery-response   the ServiceDiscoveryResponse body (no message ID)
 *   ev-model             the SensorEventIndication body with the EV model
 *   gear                 the SensorEventIndication body with gear PARK
 *   parse-discovery      read a ServiceDiscoveryRequest body on stdin and print
 *                        "device_name=<name>"
 */
#include <stdio.h>
#include <string.h>

#include "../src/aa_channels.h"
#include "../src/aa_control.h"
#include "../src/aa_sensor.h"

#define AA_DUMP_MAX_INPUT 512u

static void write_bytes(const uint8_t *data, size_t len)
{
    if (len != 0u)
        (void)fwrite(data, 1u, len, stdout);
}

static int mode_discovery_response(void)
{
    aa_control control;
    aa_control_out out;
    aa_control_reset(&control, 1u, 0u);
    if (aa_control_start(&control, &out) != AA_CONTROL_OK)
        return 1;

    /* major 1, minor 0, status MATCH */
    const uint8_t version[6] = {0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u};
    if (aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_RESPONSE, version,
                              sizeof version, &out) != AA_CONTROL_OK)
        return 1;
    if (aa_control_on_tls_established(&control, &out) != AA_CONTROL_OK)
        return 1;

    const uint8_t discovery[] = {0x22u, 0x05u, 'P', 'h', 'o', 'n', 'e'};
    if (aa_control_on_message(&control, AA_CHANNEL_CONTROL,
                              AA_CONTROL_SERVICE_DISCOVERY_REQUEST, discovery, sizeof discovery,
                              &out) != AA_CONTROL_OK)
        return 1;
    if (out.payload_len < 2u)
        return 1;

    write_bytes(out.payload + 2u, out.payload_len - 2u);
    return 0;
}

static aa_sensor_vehicle fixed_vehicle(void)
{
    aa_sensor_vehicle vehicle;
    memset(&vehicle, 0, sizeof vehicle);
    vehicle.valid = true;
    vehicle.soc_permille = 505u;
    vehicle.capacity_wh = 80000u;
    vehicle.range_m = 300000u;
    vehicle.max_charge_power_w = 150000;
    vehicle.max_discharge_power_w = 150000;
    vehicle.regen_capable = true;
    return vehicle;
}

static int mode_ev_model(void)
{
    const aa_sensor_vehicle vehicle = fixed_vehicle();
    uint8_t payload[AA_SENSOR_MAX_PAYLOAD];
    size_t payload_len = 0u;
    if (aa_sensor_encode_ev_energy(&vehicle, payload, sizeof payload, &payload_len) != AA_SENSOR_OK)
        return 1;
    if (payload_len < 2u)
        return 1;
    write_bytes(payload + 2u, payload_len - 2u);
    return 0;
}

static int mode_gear(void)
{
    uint8_t payload[AA_SENSOR_MAX_PAYLOAD];
    size_t payload_len = 0u;
    if (aa_sensor_encode_value(AA_SENSOR_VALUE_GEAR, 101, payload, sizeof payload, &payload_len) !=
        AA_SENSOR_OK)
        return 1;
    if (payload_len < 2u)
        return 1;
    write_bytes(payload + 2u, payload_len - 2u);
    return 0;
}

static int mode_parse_discovery(void)
{
    uint8_t body[AA_DUMP_MAX_INPUT];
    const size_t body_len = fread(body, 1u, sizeof body, stdin);

    aa_control control;
    aa_control_out out;
    aa_control_reset(&control, 1u, 0u);
    if (aa_control_start(&control, &out) != AA_CONTROL_OK)
        return 1;
    const uint8_t version[6] = {0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u};
    if (aa_control_on_message(&control, AA_CHANNEL_CONTROL, AA_CONTROL_VERSION_RESPONSE, version,
                              sizeof version, &out) != AA_CONTROL_OK)
        return 1;
    if (aa_control_on_tls_established(&control, &out) != AA_CONTROL_OK)
        return 1;
    if (aa_control_on_message(&control, AA_CHANNEL_CONTROL,
                              AA_CONTROL_SERVICE_DISCOVERY_REQUEST, body, body_len,
                              &out) != AA_CONTROL_OK)
        return 1;

    (void)printf("device_name=%s\n", control.device_name);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : NULL;
    if (mode == NULL || strcmp(mode, "--help") == 0) {
        (void)fprintf(stderr,
                      "usage: aa_dump discovery-response|ev-model|gear|parse-discovery\n");
        return 0;
    }
    if (strcmp(mode, "discovery-response") == 0)
        return mode_discovery_response();
    if (strcmp(mode, "ev-model") == 0)
        return mode_ev_model();
    if (strcmp(mode, "gear") == 0)
        return mode_gear();
    if (strcmp(mode, "parse-discovery") == 0)
        return mode_parse_discovery();
    (void)fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
