/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone tests for the sensor channel: the repeated channel open, the
 * sensor start handshake, the simple value events with fixed vectors, the
 * range gates, and the EV energy model parsed back field by field.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static bool payload_is(const uint8_t *payload, size_t payload_len, const char *hex)
{
    uint8_t expected[64];
    const size_t expected_len = hex_to_bytes(hex, expected, sizeof expected);
    return expected_len == payload_len && memcmp(payload, expected, expected_len) == 0;
}

static void test_channel_and_start(void)
{
    aa_sensor sensor;
    aa_sensor_out out;
    aa_sensor_reset(&sensor);

    check("channel open",
          aa_sensor_on_message(&sensor, AA_CONTROL_CHANNEL_OPEN_REQUEST, NULL, 0u, &out) ==
              AA_SENSOR_OK);
    check("channel open reply", payload_is(out.payload, out.payload_len, "00080800"));
    check("channel open flagged", sensor.channel_open_answered);

    uint8_t body[16];
    size_t body_len = hex_to_bytes("081710e807", body, sizeof body); /* type 23, 1000 ms */
    check("start request",
          aa_sensor_on_message(&sensor, AA_SENSOR_START_REQUEST, body, body_len, &out) ==
              AA_SENSOR_OK);
    check("start reply", payload_is(out.payload, out.payload_len, "80020800"));
    check("start flags", sensor.started && sensor.ev_energy_requested);
    check("start interval", sensor.refresh_interval_ms == 1000);
    check("start count", sensor.start_requests == 1u);

    /* A start request for another sensor type must not claim EV energy. */
    aa_sensor fresh;
    aa_sensor_reset(&fresh);
    body_len = hex_to_bytes("0808", body, sizeof body); /* gear only */
    check("start gear",
          aa_sensor_on_message(&fresh, AA_SENSOR_START_REQUEST, body, body_len, &out) ==
              AA_SENSOR_OK);
    check("start gear not ev", fresh.started && !fresh.ev_energy_requested);

    /* Missing type and malformed bodies are parse errors. */
    aa_sensor_reset(&fresh);
    body_len = hex_to_bytes("10e807", body, sizeof body);
    check("start without type",
          aa_sensor_on_message(&fresh, AA_SENSOR_START_REQUEST, body, body_len, &out) ==
              AA_SENSOR_ERR_PARSE);
    body_len = hex_to_bytes("0817ff", body, sizeof body);
    check("start malformed",
          aa_sensor_on_message(&fresh, AA_SENSOR_START_REQUEST, body, body_len, &out) ==
              AA_SENSOR_ERR_PARSE);
    check("start errors counted", fresh.errors == 2u);

    /* Unknown message IDs are tolerated. */
    check("unknown message",
          aa_sensor_on_message(&fresh, 0x1234u, NULL, 0u, &out) == AA_SENSOR_OK);
    check("unknown counted", fresh.ignored == 1u && out.payload_len == 0u);
}

static void test_value_vectors(void)
{
    uint8_t payload[AA_SENSOR_MAX_PAYLOAD];
    size_t payload_len = 0u;

    check("gear park",
          aa_sensor_encode_value(AA_SENSOR_VALUE_GEAR, 101, payload, sizeof payload, &payload_len) ==
              AA_SENSOR_OK);
    check("gear park vector", payload_is(payload, payload_len, "800342020865"));

    check("night mode",
          aa_sensor_encode_value(AA_SENSOR_VALUE_NIGHT_MODE, 1, payload, sizeof payload,
                                 &payload_len) == AA_SENSOR_OK);
    check("night mode vector", payload_is(payload, payload_len, "800352020801"));

    check("parking brake",
          aa_sensor_encode_value(AA_SENSOR_VALUE_PARKING_BRAKE, 1, payload, sizeof payload,
                                 &payload_len) == AA_SENSOR_OK);
    check("parking brake vector", payload_is(payload, payload_len, "80033a020801"));

    check("driving status",
          aa_sensor_encode_value(AA_SENSOR_VALUE_DRIVING_STATUS, 8, payload, sizeof payload,
                                 &payload_len) == AA_SENSOR_OK);
    check("driving status vector", payload_is(payload, payload_len, "80036a020808"));

    check("speed",
          aa_sensor_encode_value(AA_SENSOR_VALUE_SPEED, 50, payload, sizeof payload, &payload_len) ==
              AA_SENSOR_OK);
    check("speed vector", payload_is(payload, payload_len, "80031a020832"));

    /* Range gates keep wrong values local instead of misinforming the phone. */
    check("gear range",
          aa_sensor_encode_value(AA_SENSOR_VALUE_GEAR, 103, payload, sizeof payload, &payload_len) ==
              AA_SENSOR_ERR_RANGE);
    check("driving range", aa_sensor_encode_value(AA_SENSOR_VALUE_DRIVING_STATUS, 32, payload,
                                                  sizeof payload, &payload_len) ==
                               AA_SENSOR_ERR_RANGE);
    check("night range",
          aa_sensor_encode_value(AA_SENSOR_VALUE_NIGHT_MODE, 2, payload, sizeof payload,
                                 &payload_len) == AA_SENSOR_ERR_RANGE);
    check("speed range",
          aa_sensor_encode_value(AA_SENSOR_VALUE_SPEED, -1, payload, sizeof payload, &payload_len) ==
              AA_SENSOR_ERR_RANGE);
}

static void test_remaining_wh(void)
{
    aa_sensor_vehicle vehicle;
    memset(&vehicle, 0, sizeof vehicle);
    vehicle.valid = true;
    vehicle.capacity_wh = 80000u;
    vehicle.soc_permille = 505u;
    check("remaining", aa_sensor_remaining_wh(&vehicle) == 40400);

    vehicle.soc_permille = 1001u;
    check("remaining soc range", aa_sensor_remaining_wh(&vehicle) == -1);
    vehicle.soc_permille = 500u;
    vehicle.capacity_wh = 0u;
    check("remaining capacity zero", aa_sensor_remaining_wh(&vehicle) == -1);
    vehicle.capacity_wh = AA_SENSOR_MAX_CAPACITY_WH + 1u;
    check("remaining capacity bound", aa_sensor_remaining_wh(&vehicle) == -1);
    vehicle.valid = false;
    check("remaining invalid", aa_sensor_remaining_wh(&vehicle) == -1);
}

typedef struct {
    int32_t current_wh;
    int32_t capacity_wh;
    int32_t reserve_wh;
    int32_t charge_w;
    int32_t discharge_w;
    bool regen;
    float rate;
    int32_t prefs_mode;
    bool saw_model;
} parsed_ev;

static bool read_energy_value(aa_proto_iter *iter, int32_t *out)
{
    const uint8_t *data = NULL;
    size_t len = 0u;
    if (aa_proto_read_len(iter, &data, &len) != AA_PROTO_OK)
        return false;
    aa_proto_iter sub;
    aa_proto_iter_init(&sub, data, len);
    uint32_t field = 0u;
    uint8_t wire = 0u;
    if (aa_proto_next(&sub, &field, &wire) != AA_PROTO_OK || field != 1u ||
        wire != AA_PROTO_WIRE_VARINT)
        return false;
    uint64_t value = 0u;
    if (aa_proto_read_varint(&sub, &value) != AA_PROTO_OK)
        return false;
    *out = (int32_t)value;
    return true;
}

static bool parse_ev_energy(const uint8_t *payload, size_t payload_len, parsed_ev *out)
{
    if (payload_len < 2u || payload[0] != 0x80u || payload[1] != 0x03u)
        return false;

    aa_proto_iter iter;
    aa_proto_iter_init(&iter, payload + 2u, payload_len - 2u);
    uint32_t field = 0u;
    uint8_t wire = 0u;
    if (aa_proto_next(&iter, &field, &wire) != AA_PROTO_OK ||
        field != AA_SENSOR_EV_ENERGY_MODEL_TYPE || wire != AA_PROTO_WIRE_LEN)
        return false;

    const uint8_t *model = NULL;
    size_t model_len = 0u;
    if (aa_proto_read_len(&iter, &model, &model_len) != AA_PROTO_OK)
        return false;
    out->saw_model = true;

    aa_proto_iter model_iter;
    aa_proto_iter_init(&model_iter, model, model_len);
    for (;;) {
        uint32_t model_field = 0u;
        uint8_t model_wire = 0u;
        if (aa_proto_next(&model_iter, &model_field, &model_wire) != AA_PROTO_OK)
            break;

        if (model_field == 1u && model_wire == AA_PROTO_WIRE_LEN) {
            const uint8_t *battery = NULL;
            size_t battery_len = 0u;
            if (aa_proto_read_len(&model_iter, &battery, &battery_len) != AA_PROTO_OK)
                return false;
            aa_proto_iter battery_iter;
            aa_proto_iter_init(&battery_iter, battery, battery_len);
            for (;;) {
                uint32_t battery_field = 0u;
                uint8_t battery_wire = 0u;
                if (aa_proto_next(&battery_iter, &battery_field, &battery_wire) != AA_PROTO_OK)
                    break;
                if (battery_field == 3u)
                    read_energy_value(&battery_iter, &out->current_wh);
                else if (battery_field == 4u)
                    read_energy_value(&battery_iter, &out->capacity_wh);
                else if (battery_field == 8u)
                    read_energy_value(&battery_iter, &out->reserve_wh);
                else if (battery_field == 9u) {
                    uint64_t value = 0u;
                    aa_proto_read_varint(&battery_iter, &value);
                    out->charge_w = (int32_t)value;
                } else if (battery_field == 10u) {
                    uint64_t value = 0u;
                    aa_proto_read_varint(&battery_iter, &value);
                    out->discharge_w = (int32_t)value;
                } else if (battery_field == 11u) {
                    uint64_t value = 0u;
                    aa_proto_read_varint(&battery_iter, &value);
                    out->regen = value == 1u;
                } else if (aa_proto_skip(&battery_iter, battery_wire) != AA_PROTO_OK)
                    return false;
            }
            continue;
        }

        if (model_field == 2u && model_wire == AA_PROTO_WIRE_LEN) {
            const uint8_t *consumption = NULL;
            size_t consumption_len = 0u;
            if (aa_proto_read_len(&model_iter, &consumption, &consumption_len) != AA_PROTO_OK)
                return false;
            aa_proto_iter consumption_iter;
            aa_proto_iter_init(&consumption_iter, consumption, consumption_len);
            uint32_t c_field = 0u;
            uint8_t c_wire = 0u;
            if (aa_proto_next(&consumption_iter, &c_field, &c_wire) == AA_PROTO_OK &&
                c_field == 1u) {
                const uint8_t *rate = NULL;
                size_t rate_len = 0u;
                if (aa_proto_read_len(&consumption_iter, &rate, &rate_len) != AA_PROTO_OK)
                    return false;
                aa_proto_iter rate_iter;
                aa_proto_iter_init(&rate_iter, rate, rate_len);
                uint32_t r_field = 0u;
                uint8_t r_wire = 0u;
                if (aa_proto_next(&rate_iter, &r_field, &r_wire) == AA_PROTO_OK && r_field == 1u) {
                    uint32_t bits = 0u;
                    if (aa_proto_read_fixed32(&rate_iter, &bits) != AA_PROTO_OK)
                        return false;
                    memcpy(&out->rate, &bits, sizeof bits);
                }
            }
            continue;
        }

        if (model_field == 12u && model_wire == AA_PROTO_WIRE_LEN) {
            const uint8_t *prefs = NULL;
            size_t prefs_len = 0u;
            if (aa_proto_read_len(&model_iter, &prefs, &prefs_len) != AA_PROTO_OK)
                return false;
            aa_proto_iter prefs_iter;
            aa_proto_iter_init(&prefs_iter, prefs, prefs_len);
            uint32_t p_field = 0u;
            uint8_t p_wire = 0u;
            if (aa_proto_next(&prefs_iter, &p_field, &p_wire) == AA_PROTO_OK && p_field == 3u) {
                uint64_t value = 0u;
                if (aa_proto_read_varint(&prefs_iter, &value) == AA_PROTO_OK)
                    out->prefs_mode = (int32_t)value;
            }
            continue;
        }

        if (aa_proto_skip(&model_iter, model_wire) != AA_PROTO_OK)
            return false;
    }
    return true;
}

static void test_ev_energy(void)
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

    uint8_t payload[AA_SENSOR_MAX_PAYLOAD];
    size_t payload_len = 0u;
    check("ev energy encode",
          aa_sensor_encode_ev_energy(&vehicle, payload, sizeof payload, &payload_len) == AA_SENSOR_OK);

    parsed_ev parsed;
    memset(&parsed, 0, sizeof parsed);
    check("ev energy parses", parse_ev_energy(payload, payload_len, &parsed));
    check("ev energy model present", parsed.saw_model);
    check("ev current energy", parsed.current_wh == 40400);
    check("ev capacity", parsed.capacity_wh == 80000);
    check("ev reserve", parsed.reserve_wh == 4000);
    check("ev charge power", parsed.charge_w == 150000);
    check("ev discharge power", parsed.discharge_w == 150000);
    check("ev regen", parsed.regen);
    check("ev consumption rate", fabsf(parsed.rate - 134.6667f) < 0.01f);
    check("ev charging prefs", parsed.prefs_mode == 1);

    /* An unknown range must not invent a consumption rate. */
    vehicle.range_m = 0u;
    memset(&parsed, 0, sizeof parsed);
    check("ev energy no range",
          aa_sensor_encode_ev_energy(&vehicle, payload, sizeof payload, &payload_len) == AA_SENSOR_OK);
    check("ev energy no range parses", parse_ev_energy(payload, payload_len, &parsed));
    check("ev rate zero", parsed.rate == 0.0f);

    /* Out-of-range state is refused instead of sent. */
    vehicle.soc_permille = 1001u;
    check("ev soc range",
          aa_sensor_encode_ev_energy(&vehicle, payload, sizeof payload, &payload_len) ==
              AA_SENSOR_ERR_RANGE);
    vehicle.soc_permille = 500u;
    vehicle.valid = false;
    check("ev invalid",
          aa_sensor_encode_ev_energy(&vehicle, payload, sizeof payload, &payload_len) ==
              AA_SENSOR_ERR_RANGE);

    check("status names", strcmp(aa_sensor_status_name(AA_SENSOR_ERR_RANGE), "range") == 0);
}

int main(void)
{
    test_channel_and_start();
    test_value_vectors();
    test_remaining_wh();
    test_ev_energy();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("aa_sensor: all checks passed\n");
    return 0;
}
