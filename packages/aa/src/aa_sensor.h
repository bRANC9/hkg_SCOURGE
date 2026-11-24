/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Sensor channel of the AA core: vehicle data toward the phone.
 *
 * This is where the head unit tells the phone what the car is doing, and it is
 * the only place battery state of charge can reach Google Maps.  A transparent
 * wireless dongle cannot do this, which is why this module exists.
 *
 * Message shapes, pinned against aasdk commit
 * 046b3b381595509d0939fa84b14a90978f46ff63 (read-only, nothing copied):
 *   sensor channel open   ChannelOpenRequest { priority = 1, channel_id = 2 }
 *                         arriving on the sensor channel itself, answered with
 *                         ChannelOpenResponse { status = 1 };
 *   sensor start          0x8001 SensorStartRequestMessage { sensor_type = 1,
 *                         refresh_interval = 2 }, answered with 0x8002
 *                         SensorStartResponseMessage { status = 1 };
 *   sensor event          0x8003 SensorEventIndication, whose field number is
 *                         the sensor type itself: parking_brake = 7,
 *                         gear = 8, night_mode = 10, driving_status = 13,
 *                         speed = 3, with the value messages
 *                         Gear { gear = 1 }, DrivingStatus { status = 1 },
 *                         NightMode { is_night = 1 },
 *                         ParkingBrake { parking_brake = 1 },
 *                         Speed { speed = 1 }.
 *
 * EV energy model, sensor type 23.  Types 23 to 26 are not in the reference
 * enum; they are observed in the Android Auto phone APK and documented by the
 * openautolink EV work: sensor 23 carries the vehicle energy model protobuf,
 * with the wrapper serialized as pass-through, so the sensor payload *is* the
 * model bytes.  Only the fields with real-car evidence are emitted here:
 *   VehicleEnergyModel {
 *     battery = 1 { min_usable_capacity = 3, max_capacity = 4,
 *                   reserve_energy = 8, max_charge_power_w = 9,
 *                   max_discharge_power_w = 10, regen_braking_capable = 11 },
 *     consumption = 2 { driving = 1 { rate = 1 } },
 *     charging_prefs = 12 { mode = 3 }
 *   }
 *   EnergyValue { watt_hours = 1 }, EnergyRate { rate = 1 (float) }.
 *
 * Two facts that are easy to get wrong, both from the Maps decompile:
 *   - `min_usable_capacity` is read by Maps as the *current* battery energy,
 *     not as a floor, so it must carry the live value;
 *   - the driving consumption rate is Wh per km, derived from the car's own
 *     remaining range, not a hardcoded constant.
 * The remaining reconstructed fields of the model (charging curves, thermal,
 * wheels, calibration) are deliberately not emitted: their names and types are
 * not established, and inventing them would be fabricated vehicle data.
 *
 * Nothing here allocates, logs, reads configuration or performs I/O.
 */

#ifndef AA_SENSOR_H
#define AA_SENSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aa_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AA_SENSOR_EV_ENERGY_MODEL_TYPE 23u
#define AA_SENSOR_MAX_PAYLOAD 512u
#define AA_SENSOR_MAX_CAPACITY_WH 500000u /* local sanity bound: 500 kWh */

/* Sensor-channel message IDs (0x8000 range, unlike the control channel). */
#define AA_SENSOR_START_REQUEST 0x8001u
#define AA_SENSOR_START_RESPONSE 0x8002u
#define AA_SENSOR_EVENT_INDICATION 0x8003u

typedef enum {
    AA_SENSOR_OK = 0,
    AA_SENSOR_ERR_ARGUMENT = -1,
    AA_SENSOR_ERR_STATE = -2,
    AA_SENSOR_ERR_PARSE = -3,
    AA_SENSOR_ERR_BUFFER = -4,
    AA_SENSOR_ERR_RANGE = -5
} aa_sensor_status;

/* Values this tree can send today.  Each maps to the SensorEventIndication
 * field whose number equals the sensor type. */
typedef enum {
    AA_SENSOR_VALUE_PARKING_BRAKE = 0,
    AA_SENSOR_VALUE_GEAR = 1,
    AA_SENSOR_VALUE_NIGHT_MODE = 2,
    AA_SENSOR_VALUE_DRIVING_STATUS = 3,
    AA_SENSOR_VALUE_SPEED = 4
} aa_sensor_value;

typedef struct {
    bool channel_open_answered;
    bool ev_energy_requested;
    bool started;
    int64_t refresh_interval_ms;
    uint32_t start_requests;
    uint32_t ignored;
    uint32_t errors;
} aa_sensor;

/* Vehicle state, filled by whoever owns the car data (the Battery package or
 * the bridge's CAN reader).  soc_permille is 0..1000 so a 0.5 % source fits. */
typedef struct {
    bool valid;
    uint16_t soc_permille;
    uint32_t capacity_wh;
    uint32_t range_m;
    int32_t max_charge_power_w;
    int32_t max_discharge_power_w;
    bool regen_capable;
} aa_sensor_vehicle;

typedef struct {
    uint8_t payload[AA_SENSOR_MAX_PAYLOAD];
    size_t payload_len;
    bool is_event;
    aa_sensor_status status;
} aa_sensor_out;

void aa_sensor_reset(aa_sensor *sensor);

/* Consume one sensor-channel message body (after the message ID). */
aa_sensor_status aa_sensor_on_message(aa_sensor *sensor, uint16_t message_id, const uint8_t *body,
                                      size_t body_len, aa_sensor_out *out);

/* Encode one SensorEventIndication carrying a simple value.  raw is the wire
 * value: the gear enum, the driving-status flags, 0/1 for the booleans, or
 * km/h for speed. */
aa_sensor_status aa_sensor_encode_value(aa_sensor_value kind, int32_t raw, uint8_t *payload,
                                        size_t payload_cap, size_t *payload_len);

/* Encode the EV energy model event.  Nothing is emitted when the vehicle state
 * is not valid or out of range: a fabricated SoC would be worse than none. */
aa_sensor_status aa_sensor_encode_ev_energy(const aa_sensor_vehicle *vehicle, uint8_t *payload,
                                            size_t payload_cap, size_t *payload_len);

/* Current energy in Wh derived from SoC and capacity, or -1 when invalid. */
int32_t aa_sensor_remaining_wh(const aa_sensor_vehicle *vehicle);

const char *aa_sensor_status_name(aa_sensor_status status);

#ifdef __cplusplus
}
#endif

#endif /* AA_SENSOR_H */
