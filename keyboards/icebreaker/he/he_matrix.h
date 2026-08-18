/* Copyright 2024 Serene Industries (recovered pin map)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Icebreaker HE — analog (Hall effect) matrix driver.
 *
 * Recovered by disassembling the original DFU flash dump. The matrix is 5 rows
 * x 16 columns of Hall sensors, multiplexed through five 74HC4067 16:1 analog
 * muxes into a single ADC input (PA3 / ADC1_IN3).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "quantum.h"

// One sensor in the recovered 69-entry table.
//   row  : logical matrix row (0-4)
//   col  : logical matrix col (0-15)
//   id   : physical sensor id (0-68)
//   mux  : index into the CE (chip-enable) line table (0-4)
//   addr : 4-bit S0-S3 channel select within that mux (0-15)
typedef struct {
    uint8_t row;
    uint8_t col;
    uint8_t id;
    uint8_t mux;
    uint8_t addr;
} he_sensor_t;

// Actuation modes (recovered custom value ID 6). RAM-only in the original
// firmware — resets to Normal on reboot.
typedef enum {
    HE_MODE_NORMAL        = 0,
    HE_MODE_RAPID_TRIGGER = 1,
    HE_MODE_KEY_CANCEL    = 2,
} he_actuation_mode_t;

// Per-key working configuration (RAM). Carries the rapid-trigger state machine
// (travel / peak / valley) alongside the recovered thresholds.
typedef struct {
    uint8_t  actuation;    // % travel that registers a press
    uint8_t  release;      // % travel that releases the key
    uint16_t raw_min;      // resting / noise floor (from calibration)
    uint16_t raw_max;      // fully pressed (from calibration)
    uint8_t  travel;       // last computed travel % (0..100)
    uint8_t  peak;         // peak travel since last press  (rapid trigger)
    uint8_t  valley;       // valley travel since last release (rapid trigger)
    bool     pressed;      // reported press state (post-SOCD, matrix output)
} he_key_config_t;

// Global rapid-trigger tuning (recovered custom value IDs 7/8/9). RAM-only,
// like the actuation mode — the original never persists these.
typedef struct {
    uint8_t deadzone;      // rapid-trigger "rest" band, % travel (ID 7)
    uint8_t engage;        // downward travel to re-press, % (ID 8)
    uint8_t release_dist;  // upward travel to release, % (ID 9)
} he_tuning_t;

// Recovered 6-byte on-EEPROM record (69 of these = 414 B total). Only
// `actuation`/`release` are ever updated (VIA save, ID 3); the remaining four
// bytes keep their recovered init defaults (reserved=0, engage=10, raw=0x02BC).
typedef struct __attribute__((packed)) {
    uint8_t  actuation;
    uint8_t  release;
    uint8_t  reserved;
    uint8_t  engage;
    uint16_t raw;
} he_eeprom_key_config_t;

// The recovered sensor table (defined in he_matrix.c).
extern const he_sensor_t he_sensors[HE_SENSOR_COUNT];

// Select a mux channel and perform one ADC conversion.
uint16_t he_read_sensor(uint8_t index);
uint16_t he_read_sensor_raw(const he_sensor_t *s);

// Runtime config access for the VIA handler (he_via.c).
he_key_config_t *he_get_config(uint8_t index);
void              he_set_actuation(uint8_t value);
void              he_set_release(uint8_t value);
void              he_set_all_actuation(uint8_t value);

// Actuation mode + rapid-trigger tuning (RAM-only, see he_tuning_t).
he_actuation_mode_t he_get_mode(void);
void                he_set_mode(he_actuation_mode_t mode);
uint8_t             he_get_deadzone(void);
void                he_set_deadzone(uint8_t value);
uint8_t             he_get_engage(void);
void                he_set_engage(uint8_t value);
uint8_t             he_get_release_dist(void);
void                he_set_release_dist(uint8_t value);

// Diagnostic logging level selected by QK_KB_3..QK_KB_8 (see he_handle_keycode).
uint8_t he_get_logging_mode(void);

// Recovered custom keycodes (QK_KB_0 .. QK_KB_8). The original firmware puts
// these on layer 2 (the settings layer) and handles them in the keyboard-level
// process_record handler @ flash 0x0800abe4:
//
//   QK_KB_0  Actuation Point Control (APC) mode   -> HE_MODE_NORMAL
//   QK_KB_1  Rapid Trigger (RT) mode              -> HE_MODE_RAPID_TRIGGER
//   QK_KB_2  Key Cancel (SOCD) mode               -> HE_MODE_KEY_CANCEL
//   QK_KB_3..QK_KB_8  Diagnostic logging level 0..5
//
// The handler acts on key press only and returns false (consumed) for every
// QK_KB keycode; keymaps wire this into their process_record_user().
bool he_handle_keycode(uint16_t keycode, keyrecord_t *record);

// Noise-floor calibration (recovered start/end buttons, VIA IDs 4/5).
void he_start_calibration(void);
void he_end_calibration(void);
bool he_is_calibrating(void);

// EEPROM persistence of the per-key thresholds (actuation/release).
void he_load_from_eeprom(void);
void he_save_to_eeprom(void);
