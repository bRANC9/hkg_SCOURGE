/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Sensor channel implementation.  See aa_sensor.h for the pinned message
 * shapes, the EV energy model fields that are actually established, and the
 * two facts that are easy to get wrong.
 */
#include "aa_sensor.h"

#include <string.h>

#include "aa_control.h"
#include "aa_proto.h"

static void write_u16_be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static void begin_payload(aa_sensor_out *out, uint16_t message_id)
{
    memset(out->payload, 0, 2u);
    write_u16_be(out->payload, message_id);
    out->payload_len = 2u;
    out->is_event = message_id == AA_SENSOR_EVENT_INDICATION;
    out->status = AA_SENSOR_OK;
}

static aa_sensor_status reply_status(aa_sensor_out *out, uint16_t message_id, uint32_t status)
{
    aa_proto_writer writer;
    begin_payload(out, message_id);
    aa_proto_writer_init(&writer, out->payload + 2u, AA_SENSOR_MAX_PAYLOAD - 2u);
    if (aa_proto_put_varint_field(&writer, 1u, status) != AA_PROTO_OK) {
        out->payload_len = 0u;
        return AA_SENSOR_ERR_BUFFER;
    }
    out->payload_len = 2u + aa_proto_writer_len(&writer);
    return AA_SENSOR_OK;
}

void aa_sensor_reset(aa_sensor *sensor)
{
    if (sensor == NULL)
        return;
    memset(sensor, 0, sizeof *sensor);
}

aa_sensor_status aa_sensor_on_message(aa_sensor *sensor, uint16_t message_id, const uint8_t *body,
                                      size_t body_len, aa_sensor_out *out)
{
    if (sensor == NULL || out == NULL)
        return AA_SENSOR_ERR_ARGUMENT;
    if (body == NULL && body_len != 0u)
        return AA_SENSOR_ERR_ARGUMENT;

    out->payload_len = 0u;
    out->is_event = false;
    out->status = AA_SENSOR_OK;

    switch (message_id) {
    case AA_CONTROL_CHANNEL_OPEN_REQUEST: {
        /* The service channel repeats the open handshake on its own channel. */
        const aa_sensor_status status = reply_status(out, AA_CONTROL_CHANNEL_OPEN_RESPONSE, 0u);
        if (status != AA_SENSOR_OK) {
            sensor->errors++;
            out->status = status;
            return status;
        }
        sensor->channel_open_answered = true;
        return AA_SENSOR_OK;
    }

    case AA_SENSOR_START_REQUEST: {
        aa_proto_iter iter;
        aa_proto_iter_init(&iter, body, body_len);
        uint64_t sensor_type = 0u;
        uint64_t refresh_interval = 0u;
        bool have_type = false;

        for (;;) {
            uint32_t field = 0u;
            uint8_t wire = 0u;
            const aa_proto_status proto_status = aa_proto_next(&iter, &field, &wire);
            if (proto_status == AA_PROTO_END)
                break;
            if (proto_status != AA_PROTO_OK) {
                sensor->errors++;
                out->status = AA_SENSOR_ERR_PARSE;
                return AA_SENSOR_ERR_PARSE;
            }
            if ((field == 1u || field == 2u) && wire == AA_PROTO_WIRE_VARINT) {
                uint64_t value = 0u;
                if (aa_proto_read_varint(&iter, &value) != AA_PROTO_OK) {
                    sensor->errors++;
                    out->status = AA_SENSOR_ERR_PARSE;
                    return AA_SENSOR_ERR_PARSE;
                }
                if (field == 1u) {
                    sensor_type = value;
                    have_type = true;
                } else {
                    refresh_interval = value;
                }
                continue;
            }
            if (aa_proto_skip(&iter, wire) != AA_PROTO_OK) {
                sensor->errors++;
                out->status = AA_SENSOR_ERR_PARSE;
                return AA_SENSOR_ERR_PARSE;
            }
        }

        if (!have_type) {
            sensor->errors++;
            out->status = AA_SENSOR_ERR_PARSE;
            return AA_SENSOR_ERR_PARSE;
        }

        sensor->started = true;
        sensor->start_requests++;
        sensor->refresh_interval_ms = (int64_t)refresh_interval;
        if (sensor_type == AA_SENSOR_EV_ENERGY_MODEL_TYPE)
            sensor->ev_energy_requested = true;

        const aa_sensor_status status = reply_status(out, AA_SENSOR_START_RESPONSE, 0u);
        if (status != AA_SENSOR_OK) {
            sensor->errors++;
            out->status = status;
            return status;
        }
        return AA_SENSOR_OK;
    }

    default:
        sensor->ignored++;
        return AA_SENSOR_OK;
    }
}

static aa_sensor_status value_field(aa_sensor_value kind, uint32_t *field_number)
{
    switch (kind) {
    case AA_SENSOR_VALUE_PARKING_BRAKE:
        *field_number = 7u;
        return AA_SENSOR_OK;
    case AA_SENSOR_VALUE_GEAR:
        *field_number = 8u;
        return AA_SENSOR_OK;
    case AA_SENSOR_VALUE_NIGHT_MODE:
        *field_number = 10u;
        return AA_SENSOR_OK;
    case AA_SENSOR_VALUE_DRIVING_STATUS:
        *field_number = 13u;
        return AA_SENSOR_OK;
    case AA_SENSOR_VALUE_SPEED:
        *field_number = 3u;
        return AA_SENSOR_OK;
    }
    return AA_SENSOR_ERR_ARGUMENT;
}

aa_sensor_status aa_sensor_encode_value(aa_sensor_value kind, int32_t raw, uint8_t *payload,
                                        size_t payload_cap, size_t *payload_len)
{
    if (payload == NULL || payload_len == NULL)
        return AA_SENSOR_ERR_ARGUMENT;
    if (payload_cap < 2u)
        return AA_SENSOR_ERR_BUFFER;

    uint32_t field = 0u;
    if (value_field(kind, &field) != AA_SENSOR_OK)
        return AA_SENSOR_ERR_ARGUMENT;

    /* Range gates: a wrong enum or a negative speed would misinform the phone
     * instead of simply being wrong here. */
    switch (kind) {
    case AA_SENSOR_VALUE_GEAR:
        if (raw < 0 || raw > 102)
            return AA_SENSOR_ERR_RANGE;
        break;
    case AA_SENSOR_VALUE_DRIVING_STATUS:
        if (raw < 0 || raw > 31)
            return AA_SENSOR_ERR_RANGE;
        break;
    case AA_SENSOR_VALUE_NIGHT_MODE:
    case AA_SENSOR_VALUE_PARKING_BRAKE:
        if (raw != 0 && raw != 1)
            return AA_SENSOR_ERR_RANGE;
        break;
    case AA_SENSOR_VALUE_SPEED:
        if (raw < 0)
            return AA_SENSOR_ERR_RANGE;
        break;
    }

    write_u16_be(payload, AA_SENSOR_EVENT_INDICATION);
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, payload + 2u, payload_cap - 2u);

    /* SensorEventIndication.<type> is a message whose single field 1 carries
     * the value, so exactly one nesting level is needed here. */
    size_t indicator = 0u;
    if (aa_proto_begin_message(&writer, field, &indicator) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, (uint64_t)raw) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, indicator) != AA_PROTO_OK)
        goto fail;

    *payload_len = 2u + aa_proto_writer_len(&writer);
    return AA_SENSOR_OK;

fail:
    *payload_len = 0u;
    return AA_SENSOR_ERR_BUFFER;
}

int32_t aa_sensor_remaining_wh(const aa_sensor_vehicle *vehicle)
{
    if (vehicle == NULL || !vehicle->valid)
        return -1;
    if (vehicle->soc_permille > 1000u || vehicle->capacity_wh == 0u ||
        vehicle->capacity_wh > AA_SENSOR_MAX_CAPACITY_WH)
        return -1;
    return (int32_t)((uint64_t)vehicle->capacity_wh * vehicle->soc_permille / 1000u);
}

aa_sensor_status aa_sensor_encode_ev_energy(const aa_sensor_vehicle *vehicle, uint8_t *payload,
                                            size_t payload_cap, size_t *payload_len)
{
    if (vehicle == NULL || payload == NULL || payload_len == NULL)
        return AA_SENSOR_ERR_ARGUMENT;
    const int32_t remaining_wh = aa_sensor_remaining_wh(vehicle);
    if (remaining_wh < 0)
        return AA_SENSOR_ERR_RANGE;
    if (payload_cap < 2u)
        return AA_SENSOR_ERR_BUFFER;

    const uint32_t capacity_wh = vehicle->capacity_wh;
    const uint32_t reserve_wh = capacity_wh / 20u;
    /* Wh per km from the car's own remaining range; zero when the range is
     * unknown, because a fabricated rate would skew every estimate. */
    float consumption_wh_per_km = 0.0f;
    if (vehicle->range_m != 0u)
        consumption_wh_per_km =
            (float)remaining_wh * 1000.0f / (float)vehicle->range_m;

    write_u16_be(payload, AA_SENSOR_EVENT_INDICATION);
    aa_proto_writer writer;
    aa_proto_writer_init(&writer, payload + 2u, payload_cap - 2u);

    size_t indication = 0u;
    size_t battery = 0u;
    size_t energy = 0u;
    size_t consumption = 0u;
    size_t rate = 0u;
    size_t prefs = 0u;

    if (aa_proto_begin_message(&writer, AA_SENSOR_EV_ENERGY_MODEL_TYPE, &indication) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 1u, &battery) != AA_PROTO_OK) /* battery */
        goto fail;
    if (aa_proto_begin_message(&writer, 3u, &energy) != AA_PROTO_OK) /* min_usable_capacity */
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, (uint64_t)remaining_wh) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, energy) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 4u, &energy) != AA_PROTO_OK) /* max_capacity */
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, (uint64_t)capacity_wh) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, energy) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 8u, &energy) != AA_PROTO_OK) /* reserve_energy */
        goto fail;
    if (aa_proto_put_varint_field(&writer, 1u, (uint64_t)reserve_wh) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, energy) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(&writer, 9u, (uint64_t)(vehicle->max_charge_power_w < 0
                                                             ? 0
                                                             : vehicle->max_charge_power_w)) !=
        AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_varint_field(
            &writer, 10u,
            (uint64_t)(vehicle->max_discharge_power_w < 0 ? 0 : vehicle->max_discharge_power_w)) !=
        AA_PROTO_OK)
        goto fail;
    if (aa_proto_put_bool_field(&writer, 11u, vehicle->regen_capable) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, battery) != AA_PROTO_OK)
        goto fail;

    if (aa_proto_begin_message(&writer, 2u, &consumption) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_begin_message(&writer, 1u, &rate) != AA_PROTO_OK) /* driving */
        goto fail;
    if (aa_proto_put_float_field(&writer, 1u, consumption_wh_per_km) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, rate) != AA_PROTO_OK)
        goto fail;
    if (aa_proto_end_message(&writer, consumption) != AA_PROTO_OK)
        goto fail;

    if (aa_proto_begin_message(&writer, 12u, &prefs) != AA_PROTO_OK) /* charging_prefs */
        goto fail;
    if (aa_proto_put_varint_field(&writer, 3u, 1u) != AA_PROTO_OK) /* mode: standard */
        goto fail;
    if (aa_proto_end_message(&writer, prefs) != AA_PROTO_OK)
        goto fail;

    if (aa_proto_end_message(&writer, indication) != AA_PROTO_OK)
        goto fail;

    *payload_len = 2u + aa_proto_writer_len(&writer);
    return AA_SENSOR_OK;

fail:
    *payload_len = 0u;
    return AA_SENSOR_ERR_BUFFER;
}

const char *aa_sensor_status_name(aa_sensor_status status)
{
    switch (status) {
    case AA_SENSOR_OK:
        return "ok";
    case AA_SENSOR_ERR_ARGUMENT:
        return "argument";
    case AA_SENSOR_ERR_STATE:
        return "state";
    case AA_SENSOR_ERR_PARSE:
        return "parse";
    case AA_SENSOR_ERR_BUFFER:
        return "buffer";
    case AA_SENSOR_ERR_RANGE:
        return "range";
    }
    return "unknown";
}
