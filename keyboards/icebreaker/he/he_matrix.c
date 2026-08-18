/* Copyright 2024 Serene Industries (recovered pin map)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Icebreaker HE — analog (Hall effect) matrix driver (reconstruction).
 *
 * The sensor table, mux pin map, ADC pin, and read_sensor algorithm below were
 * recovered from the original firmware (DFU flash dump disassembly):
 *
 *   - 5x 74HC4067 16:1 analog muxes, wired-OR into PA3 (ADC1_IN3).
 *   - CE (enable) lines:   [PB0, PA7, PA6, PA5, PA4]  (active-LOW)
 *   - S0-S3 address lines: [PB3, PB4, PB6, PB5]
 *   - 69-entry sensor table @ flash 0x08013BFC, 5 bytes/entry:
 *       { row, col, sensor_id, mux_ce_idx, mux_addr }
 *   - read_sensor() @ flash 0x08009850 (algorithm reproduced below).
 *
 * Implemented (previously TODO):
 *   - Rapid trigger / key-cancel (SOCD) / actuation-point control state machine.
 *   - Noise-floor calibration (recovered 10-sample-min @ 0x080098E0).
 *   - EEPROM persistence of the per-key thresholds (2-byte structs, 138 B) and
 *     the global settings (actuation mode + rapid-trigger tuning, 4 B).
 *   - 12-bit ADC group config (ADC_RESOLUTION in config.h).
 *
 * Encoder (recovered):
 *   - A/B rotation = PB14/PB13 (standard QMK encoder.c, see config.h).
 *   - Push button  = matrix [4,9], read via PB12 (drive HIGH) + PB15 (sense,
 *     pull-down). Implemented below (sensor index 68).
 */

#include "he_matrix.h"
#include "he_via.h"

#include "analog.h"
#include "eeconfig.h"
#include "gpio.h"
#include "keyboard.h"
#include "print.h"
#include "rgblight.h"

/* --------------------------------------------------------------------------
 * Bootmagic reset-loop guard
 *
 * VIA_ENABLE forces BOOTMAGIC_ENABLE=yes (builddefs/common_features.mk), so the
 * bootmagic scan runs during keyboard_init() even though keyboard.json sets
 * "bootmagic": false. The Hall sensors rest near mid-scale, and until they are
 * calibrated a spurious "pressed" reading on the bootmagic key (row 0, col 0)
 * makes bootmagic_should_reset() return true, which triggers
 * bootmagic_reset_eeprom() + bootloader_jump() -> NVIC_SystemReset() on every
 * power-up. This keyboard has no physical bootmagic key, so override the weak
 * decision hook to always decline the reset.
 * ------------------------------------------------------------------------ */
bool bootmagic_should_reset(void);
bool bootmagic_should_reset(void) {
    return false;
}

/* --------------------------------------------------------------------------
 * Recovered pin map
 * ------------------------------------------------------------------------ */

static const pin_t mux_ce_pins[HE_MUX_COUNT] = HE_MUX_CE_PINS;     // PB0 PA7 PA6 PA5 PA4
static const pin_t mux_addr_pins[HE_ADDR_LINES] = HE_MUX_ADDR_PINS; // PB3 PB4 PB6 PB5

/* --------------------------------------------------------------------------
 * Recovered sensor table — 69 entries: { row, col, id, mux, addr }
 *
 * NOTE: entry 68 ({4,9,68,0,0}) collides with entry 18 ({1,2,18,0,0}) on
 * (mux 0, addr 0). The encoder push [4,9] is therefore *not* read through the
 * mux — it is a separate switch (see readme). Kept here for completeness.
 * ------------------------------------------------------------------------ */

const he_sensor_t he_sensors[HE_SENSOR_COUNT] = {
    [ 0] = { 0, 0, 0, 0,  3 },
    [ 1] = { 0, 1, 1, 0,  5 },
    [ 2] = { 0, 2, 2, 0,  6 },
    [ 3] = { 0, 3, 3, 0,  2 },
    [ 4] = { 0, 4, 4, 1,  6 },
    [ 5] = { 0, 5, 5, 1,  5 },
    [ 6] = { 0, 6, 6, 1,  4 },
    [ 7] = { 0, 7, 7, 1,  3 },
    [ 8] = { 0, 8, 8, 2,  5 },
    [ 9] = { 0, 9, 9, 2,  4 },
    [10] = { 0, 10, 10, 2,  3 },
    [11] = { 0, 11, 11, 2,  2 },
    [12] = { 0, 12, 12, 3,  3 },
    [13] = { 0, 13, 13, 3,  2 },
    [14] = { 0, 14, 14, 4,  5 },
    [15] = { 0, 15, 15, 4,  4 },
    [16] = { 1, 0, 16, 0,  4 },
    [17] = { 1, 1, 17, 0,  7 },
    [18] = { 1, 2, 18, 0,  0 },
    [19] = { 1, 3, 19, 0,  1 },
    [20] = { 1, 4, 20, 1,  7 },
    [21] = { 1, 5, 21, 1,  0 },
    [22] = { 1, 6, 22, 1,  1 },
    [23] = { 1, 7, 23, 1,  2 },
    [24] = { 1, 8, 24, 2,  6 },
    [25] = { 1, 9, 25, 2,  0 },
    [26] = { 1, 10, 26, 2,  1 },
    [27] = { 1, 11, 27, 3,  5 },
    [28] = { 1, 12, 28, 3,  0 },
    [29] = { 1, 13, 29, 3,  1 },
    [30] = { 1, 14, 30, 4,  2 },
    [31] = { 2, 0, 31, 0,  8 },
    [32] = { 2, 1, 32, 0, 10 },
    [33] = { 2, 2, 33, 0, 15 },
    [34] = { 2, 3, 34, 1,  8 },
    [35] = { 2, 4, 35, 1, 10 },
    [36] = { 2, 5, 36, 1, 15 },
    [37] = { 2, 6, 37, 1, 14 },
    [38] = { 2, 7, 38, 2,  8 },
    [39] = { 2, 8, 39, 2,  9 },
    [40] = { 2, 9, 40, 2, 15 },
    [41] = { 2, 10, 41, 3,  8 },
    [42] = { 2, 11, 42, 3,  9 },
    [43] = { 2, 12, 43, 3, 14 },
    [44] = { 2, 13, 44, 4, 14 },
    [45] = { 3, 0, 45, 0,  9 },
    [46] = { 3, 1, 46, 0, 13 },
    [47] = { 3, 2, 47, 0, 14 },
    [48] = { 3, 3, 48, 1, 11 },
    [49] = { 3, 4, 49, 1, 12 },
    [50] = { 3, 5, 50, 2, 10 },
    [51] = { 3, 6, 51, 2, 11 },
    [52] = { 3, 7, 52, 2, 12 },
    [53] = { 3, 8, 53, 2, 13 },
    [54] = { 3, 9, 54, 2, 14 },
    [55] = { 3, 10, 55, 3, 10 },
    [56] = { 3, 11, 56, 3, 13 },
    [57] = { 3, 12, 57, 4, 10 },
    [58] = { 3, 13, 58, 4, 13 },
    [59] = { 4, 0, 59, 0, 11 },
    [60] = { 4, 1, 60, 0, 12 },
    [61] = { 4, 2, 61, 1,  9 },
    [62] = { 4, 3, 62, 1, 13 },
    [63] = { 4, 4, 63, 3, 11 },
    [64] = { 4, 5, 64, 3, 12 },
    [65] = { 4, 6, 65, 4,  9 },
    [66] = { 4, 7, 66, 4, 11 },
    [67] = { 4, 8, 67, 4, 12 },
    [68] = { 4, 9, 68, 0,  0 },
};

/* --------------------------------------------------------------------------
 * Runtime per-key config + global tuning (RAM).
 * ------------------------------------------------------------------------ */

static he_key_config_t he_config[HE_SENSOR_COUNT];
static uint16_t       he_rest_floor[HE_SENSOR_COUNT]; // per-sensor resting floor (auto-cal)
// Hardcoded boot defaults: Rapid Trigger with deadzone 50 / engage 15 /
// disengage (release distance) 10. These apply on a fresh EEPROM; a persisted
// VIA setting still overrides them on subsequent boots.
static he_actuation_mode_t he_mode = HE_MODE_RAPID_TRIGGER;
static he_tuning_t he_tuning       = {
    .deadzone     = HE_DEADZONE_DEFAULT,
    .engage       = HE_ENGAGE_DEFAULT,
    .release_dist = HE_RELEASE_DIST_DEFAULT,
};

// Recovered key-cancel (SOCD) pairs, by sensor-table index:
//   A (idx 32, matrix [2,1]) <-> D (idx 34, matrix [2,3])
//   Z (idx 46, matrix [3,1]) <-> X (idx 47, matrix [3,2])
static const uint8_t socd_pairs[][2] = {
    {32, 34},
    {46, 47},
};
#define SOCD_PAIR_COUNT (sizeof(socd_pairs) / sizeof(socd_pairs[0]))

// Recovered LED remap table @ flash 0x08013D88 — maps sensor-table index to
// the WS2812 strip position. The underglow is wired in a serpentine: rows 0, 2
// and 4 run left-to-right, rows 1 and 3 run right-to-left. Entry 68 (encoder
// push) is a dummy (0) and is never lit.
static const uint8_t he_led_remap[HE_SENSOR_COUNT] = {
    // row 0 (left-to-right)
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    // row 1 (right-to-left)
    30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
    // row 2 (left-to-right)
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
    // row 3 (right-to-left)
    58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45,
    // row 4 (left-to-right)
    59, 60, 61, 62, 63, 64, 65, 66, 67,
    // encoder push (sensor 68) — dummy, never lit
    0,
};

// Rotary-encoder push is the last sensor-table entry (68), read via GPIO
// rather than the analog mux.
#define HE_ENC_PUSH_INDEX (HE_SENSOR_COUNT - 1)

he_key_config_t *he_get_config(uint8_t index) {
    return (index < HE_SENSOR_COUNT) ? &he_config[index] : NULL;
}

// Clamp an 8-bit value into [lo, hi]. Values from the VIA raw-HID channel are
// clamped (not rejected) so an out-of-range packet degrades to the nearest
// valid bound rather than writing an unusable threshold.
static uint8_t he_clamp_u8(uint8_t value, uint8_t lo, uint8_t hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

void he_set_actuation(uint8_t value) {
    value = he_clamp_u8(value, HE_ACTUATION_MIN, HE_ACTUATION_MAX);
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        he_config[i].actuation = value;
    }
}

void he_set_release(uint8_t value) {
    value = he_clamp_u8(value, HE_RELEASE_MIN, HE_RELEASE_MAX);
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        he_config[i].release = value;
    }
}

void he_set_all_actuation(uint8_t value) {
    he_set_actuation(value);
}

he_actuation_mode_t he_get_mode(void) {
    return he_mode;
}

void he_set_mode(he_actuation_mode_t mode) {
    if (mode <= HE_MODE_KEY_CANCEL) {
        he_mode = mode;
        // The original kept the mode RAM-only, so it reset to Normal on every
        // power-cycle. Persist immediately so a mode change survives reboot
        // (discrete setting, no slider wear concern).
        he_save_settings_to_eeprom();
    }
}

// Rapid-trigger tuning setters update RAM only. They are VIA slider values
// (IDs 7/8/9), so persisting on every drag would hammer the flash-backed EEPROM
// and stall the VIA command handler. They are persisted by he_save_to_eeprom()
// on the save action (VIA ID 3 or the generic id_custom_save / 0x09).
uint8_t he_get_deadzone(void) {
    return he_tuning.deadzone;
}

void he_set_deadzone(uint8_t value) {
    he_tuning.deadzone = he_clamp_u8(value, HE_DEADZONE_MIN, HE_DEADZONE_MAX);
}

uint8_t he_get_engage(void) {
    return he_tuning.engage;
}

void he_set_engage(uint8_t value) {
    he_tuning.engage = he_clamp_u8(value, HE_ENGAGE_MIN, HE_ENGAGE_MAX);
}

uint8_t he_get_release_dist(void) {
    return he_tuning.release_dist;
}

void he_set_release_dist(uint8_t value) {
    he_tuning.release_dist = he_clamp_u8(value, HE_RELEASE_DIST_MIN, HE_RELEASE_DIST_MAX);
}

/* --------------------------------------------------------------------------
 * Recovered custom keycodes (QK_KB_0 .. QK_KB_2)
 *
 * The original process_record handler (flash @ 0x0800abe4) subtracts QK_KB
 * (0x7E00) from the keycode and switches on the result (0..8), acting on key
 * press only and returning false (consumed) for every QK_KB keycode. Only the
 * three actuation-mode keycodes are implemented; the original's diagnostic
 * logging levels (QK_KB_3..8) are intentionally not reconstructed.
 *
 *   QK_KB_0 -> APC mode (actuation = 85)
 *   QK_KB_1 -> RT mode  (actuation = 0)
 *   QK_KB_2 -> Key Cancel (actuation = 170)
 *
 * The original also persists a mode-specific default threshold (85/0/170) to
 * EEPROM via a helper @ 0x0800ab90; that side effect is omitted here in favour
 * of the he_set_mode() path used by the VIA handler (which persists the mode
 * itself).
 * ------------------------------------------------------------------------ */

bool he_handle_keycode(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case QK_KB_0: // Actuation Point Control mode
            if (record->event.pressed) {
                he_set_mode(HE_MODE_NORMAL);
                uprintf("Actuation Point Control Mode set\n");
                uprintf("[PCB_SETTINGS]: APC MODE\n");
            }
            return false;
        case QK_KB_1: // Rapid Trigger mode
            if (record->event.pressed) {
                he_set_mode(HE_MODE_RAPID_TRIGGER);
                uprintf("Rapid Trigger Mode set\n");
                uprintf("[PCB_SETTINGS]: RT MODE\n");
            }
            return false;
        case QK_KB_2: // Key Cancel (SOCD) mode
            if (record->event.pressed) {
                he_set_mode(HE_MODE_KEY_CANCEL);
                uprintf("Key Cancel Mode set\n");
            }
            return false;
        default:
            return true;
    }
}

/* --------------------------------------------------------------------------
 * Mux + ADC helpers
 * ------------------------------------------------------------------------ */

// Disable every mux (all CE lines HIGH — active-LOW chips).
static void he_mux_all_disabled(void) {
    for (uint8_t i = 0; i < HE_MUX_COUNT; i++) {
        gpio_write_pin_high(mux_ce_pins[i]);
    }
}

// Enable mux `idx` and drive `addr` onto S0-S3.
static void he_mux_select(uint8_t idx, uint8_t addr) {
    // Drive the address lines while every mux is disabled (CE high), then
    // enable the selected chip. Selecting first avoids switching S0-S3 while a
    // mux output is live, which would glitch the wired-OR ADC bus.
    he_mux_all_disabled();
    for (uint8_t b = 0; b < HE_ADDR_LINES; b++) {
        if (addr & (1 << b)) {
            gpio_write_pin_high(mux_addr_pins[b]);
        } else {
            gpio_write_pin_low(mux_addr_pins[b]);
        }
    }
    if (idx < HE_MUX_COUNT) {
        gpio_write_pin_low(mux_ce_pins[idx]);
    }
}

// Read a sensor struct directly (raw 12-bit ADC value, uncalibrated).
uint16_t he_read_sensor_raw(const he_sensor_t *s) {
    he_mux_select(s->mux, s->addr);
    // Let the mux output settle before sampling (see HE_MUX_SETTLE_US).
    wait_us(HE_MUX_SETTLE_US);
    return (uint16_t)analogReadPin(HE_ADC_PIN);
}

// Read by table index.
uint16_t he_read_sensor(uint8_t index) {
    if (index >= HE_SENSOR_COUNT) {
        return 0;
    }
    return he_read_sensor_raw(&he_sensors[index]);
}

/* --------------------------------------------------------------------------
 * Encoder push button (recovered @ flash 0x08009E18)
 *
 * The rotary-encoder click is a mechanical switch wired between PB12 (drive)
 * and PB15 (sense). To read it: drive PB12 HIGH, then read PB15 (pull-down).
 * Pressed == PB15 HIGH. The original reconfigures PB12 as an output on every
 * read; we do the same for fidelity.
 * ------------------------------------------------------------------------ */

static bool he_read_encoder_push(void) {
    gpio_set_pin_output(HE_ENC_PUSH_DRIVE_PIN);
    gpio_write_pin_high(HE_ENC_PUSH_DRIVE_PIN);
    return gpio_read_pin(HE_ENC_PUSH_SENSE_PIN);
}

/* --------------------------------------------------------------------------
 * Noise-floor calibration (recovered @ flash 0x080098E0)
 *
 * The original takes 10 ADC samples per sensor and tracks the minimum as the
 * noise floor. Here we accumulate running min/max while `he_calibrating` is
 * set (between the VIA "start" and "end" buttons), so every sensor is sampled
 * across several scan passes; `he_end_calibration()` clamps any sensor whose
 * min/max span is implausibly small ("low ceiling" warning).
 * ------------------------------------------------------------------------ */

static bool he_calibrating = false;

#if defined(RGBLIGHT_ENABLE)
// Recovered behaviour: the original firmware turned the RGB strip RED while a
// VIA calibration was running, then restored the previous colour on "end".
static hsv_t cal_saved_hsv;
// Per-key latch: true once a key has been pressed during the current run (its
// LED turns green and stays green until calibration ends).
static bool he_cal_latched[HE_SENSOR_COUNT];
#endif

void he_start_calibration(void) {
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
#if defined(RGBLIGHT_ENABLE)
        he_cal_latched[i] = false;
#endif
        if (i == HE_ENC_PUSH_INDEX) {
            continue; // encoder push is a GPIO switch, not analog
        }
        he_config[i].raw_min = HE_ADC_RAW_MAX;
        he_config[i].raw_max = 0;
    }
    he_calibrating = true;
#if defined(RGBLIGHT_ENABLE)
    // Visual confirmation: save the current colour and turn the strip red.
    cal_saved_hsv = rgblight_get_hsv();
    rgblight_sethsv_noeeprom(HSV_RED);
#endif
    uprintf("Calibration started, fully press each key and end calibration in VIA.\n");
}

void he_end_calibration(void) {
    he_calibrating = false;
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        if (i == HE_ENC_PUSH_INDEX) {
            continue; // encoder push is a GPIO switch, not analog
        }
        // Reject a zero/tiny span: it means the key was never actually pressed
        // during calibration (only ADC noise was seen), not a real "low
        // ceiling". Fall back to the boot auto-cal values (resting floor +
        // measured ~700-count swing) instead of the uncalibrated mid/full-scale
        // defaults the sensor can never physically reach.
        if (he_config[i].raw_max - he_config[i].raw_min < HE_ADC_MIN_SPAN) {
            uprintf("Warning: Sensor %d low ceiling value\n", i);
            uint16_t base = HE_ADC_RAW_MID;
            if (he_rest_floor[i] < HE_ADC_RAW_MAX) {
                base = (he_rest_floor[i] > HE_ADC_REST_MARGIN) ? (he_rest_floor[i] - HE_ADC_REST_MARGIN) : 0;
            }
            he_config[i].raw_min = base;
            he_config[i].raw_max = base + HE_ADC_TRAVEL_SPAN;
        }
    }
#if defined(RGBLIGHT_ENABLE)
    // Restore the pre-calibration colour.
    rgblight_sethsv_noeeprom(cal_saved_hsv.h, cal_saved_hsv.s, cal_saved_hsv.v);
#endif
    uprintf("Calibration ended.\n");
}

bool he_is_calibrating(void) {
    return he_calibrating;
}

/* --------------------------------------------------------------------------
 * EEPROM persistence (QMK keyboard data block)
 *
 * Layout: 69 x he_eeprom_key_config_t (2 B each = 138 B) followed by one
 * he_settings_eeprom_t (4 B) = 142 B total. The per-key records store the
 * actuation/release thresholds; the trailing settings record stores the
 * actuation mode and rapid-trigger tuning that the original firmware kept
 * RAM-only (and therefore lost on reboot).
 * ------------------------------------------------------------------------ */

void he_load_from_eeprom(void) {
    // Guard against an invalid / version-mismatched block (e.g. a size bump):
    // reading it would zero out the thresholds, so keep the defaults seeded by
    // he_config_set_defaults() and (re)establish the block instead.
    if (!eeconfig_is_kb_datablock_valid()) {
        he_save_to_eeprom();
        return;
    }

    he_eeprom_key_config_t rec[HE_SENSOR_COUNT];
    eeconfig_read_kb_datablock(rec, 0, sizeof(rec));
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        // Clamp on load too: a corrupted / legacy record must not inject an
        // out-of-range threshold into the working config.
        he_config[i].actuation = he_clamp_u8(rec[i].actuation, HE_ACTUATION_MIN, HE_ACTUATION_MAX);
        he_config[i].release   = he_clamp_u8(rec[i].release, HE_RELEASE_MIN, HE_RELEASE_MAX);
    }

    he_settings_eeprom_t settings;
    eeconfig_read_kb_datablock(&settings, HE_SETTINGS_EEPROM_OFFSET, sizeof(settings));
    if (settings.actuation_mode <= HE_MODE_KEY_CANCEL) {
        he_mode = (he_actuation_mode_t)settings.actuation_mode;
    }
    he_tuning.deadzone     = he_clamp_u8(settings.deadzone, HE_DEADZONE_MIN, HE_DEADZONE_MAX);
    he_tuning.engage       = he_clamp_u8(settings.engage, HE_ENGAGE_MIN, HE_ENGAGE_MAX);
    he_tuning.release_dist = he_clamp_u8(settings.release_dist, HE_RELEASE_DIST_MIN, HE_RELEASE_DIST_MAX);
}

void he_save_to_eeprom(void) {
    he_eeprom_key_config_t rec[HE_SENSOR_COUNT];
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        rec[i].actuation = he_config[i].actuation;
        rec[i].release   = he_config[i].release;
    }
    eeconfig_update_kb_datablock(rec, 0, sizeof(rec));
    he_save_settings_to_eeprom();
}

void he_save_settings_to_eeprom(void) {
    he_settings_eeprom_t settings = {
        .actuation_mode = (uint8_t)he_mode,
        .deadzone       = he_tuning.deadzone,
        .engage         = he_tuning.engage,
        .release_dist   = he_tuning.release_dist,
    };
    eeconfig_update_kb_datablock(&settings, HE_SETTINGS_EEPROM_OFFSET, sizeof(settings));
}

// Map a raw ADC value to a 0..100 travel percentage. The live board confirmed
// the polarity: Hall sensors read higher (ADC up) as the magnet approaches, so
// raw_min is the resting floor and raw_max the fully-pressed ceiling.
static uint8_t he_raw_to_travel(const he_key_config_t *c, uint16_t raw) {
    uint16_t span = (c->raw_max > c->raw_min) ? (c->raw_max - c->raw_min) : 1;
    int32_t  pct  = ((int32_t)(raw - c->raw_min) * 100) / span;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

// Per-key press decision across the three actuation modes.
//
//   Normal        : press at >= actuation, release at <= release (hysteresis).
//   Rapid trigger : a clean-room "peak/valley" model. While held, `peak`
//                   tracks the highest travel and the key releases when travel
//                   drops `release_dist` (the "disengage" distance) below the
//                   peak, or when it falls back into the `deadzone` rest band.
//                   While released, `valley` tracks the lowest travel and the
//                   key re-presses when travel rises `engage` above the valley
//                   (or crosses the absolute `actuation` threshold). This is
//                   the *intended* rapid-trigger semantics: it is equivalent
//                   to the official's single `boundary_value` reference but
//                   uses two explicit extremes, and it avoids the official's
//                   bug where the re-press boundary was set to
//                   `release + engage` and then re-checked against
//                   `boundary + engage` (a 2x engage dead-band).
//   Key cancel    : same as Normal; SOCD pair resolution happens in the scan.
//
// Computes the *physical* press state (before any SOCD resolution) and updates
// it through `pressed`, which points at he_phys_pressed[i]. The reported state
// in c->pressed is derived later in the scan; keeping the two separate is what
// prevents the key-cancel loser from flickering on every scan.
static bool he_compute_pressed(he_key_config_t *c, bool *pressed, uint8_t travel) {
    c->travel = travel;

    switch (he_mode) {
        case HE_MODE_RAPID_TRIGGER:
            if (*pressed) {
                // Track the peak while held. Release is *relative* to the peak
                // (drop >= release_dist), or when the key drops back into the
                // rest band (<= deadzone). The normal-mode absolute `release`
                // threshold must NOT gate RT release — it sits above the
                // deadzone and would self-cancel every engage-based re-press.
                if (travel > c->peak) {
                    c->peak = travel;
                }
                uint8_t drop = c->peak - travel;
                if (travel <= he_tuning.deadzone || drop >= he_tuning.release_dist) {
                    c->valley = travel;
                    *pressed  = false;
                }
                return *pressed;
            }
            // Not pressed: track the valley, then require the key to be above
            // the deadzone rest band before any re-press can fire. Press either
            // absolutely (passed actuation) or relatively (rose by `engage`
            // from the valley).
            if (travel < c->valley) {
                c->valley = travel;
            }
            if (travel <= he_tuning.deadzone) {
                return false;
            }
            if (travel >= c->actuation || (travel - c->valley) >= he_tuning.engage) {
                c->peak   = travel;
                *pressed  = true;
                return true;
            }
            return false;

        case HE_MODE_KEY_CANCEL:
        case HE_MODE_NORMAL:
        default:
            if (*pressed) {
                *pressed = (travel > c->release);
            } else {
                *pressed = (travel >= c->actuation);
            }
            return *pressed;
    }
}

/* --------------------------------------------------------------------------
 * QMK custom matrix (lite) hooks
 * ------------------------------------------------------------------------ */

// Set every per-key config to its power-on defaults. Called from
// keyboard_pre_init_kb() so the first-boot EEPROM format persists the real
// defaults (actuation 50 / release 30) instead of the zeroed BSS values, and
// again from matrix_init_custom() (idempotent).
static void he_config_set_defaults(void) {
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        he_config[i].actuation = HE_ACTUATION_DEFAULT;
        he_config[i].release   = HE_RELEASE_DEFAULT;
        // Bipolar Hall: rest = mid-scale, press = toward full scale. Seed a
        // *provisional* span centered at mid-scale (the expected ~700-count
        // swing) rather than the full 0..4095 scale, so a real bottom-out
        // (~2842) maps to ~100% travel even during the boot rest-sampling
        // window before the true per-sensor floor locks in. Using full scale
        // here would map a bottom-out to only ~39%, dropping early keystrokes.
        he_config[i].raw_min   = HE_ADC_RAW_MID - HE_ADC_REST_MARGIN;
        he_config[i].raw_max   = he_config[i].raw_min + HE_ADC_TRAVEL_SPAN;
        he_config[i].travel    = 0;
        he_config[i].peak      = 0;
        he_config[i].valley    = 0;
        he_config[i].debounce  = 0;
        he_config[i].pressed   = false;
        he_rest_floor[i]       = HE_ADC_RAW_MAX;
    }
}

// Runs in keyboard_setup(), after eeprom_driver_init() + matrix_setup() and
// immediately before the USB driver comes up. A marker here proves the
// wear-leveling EEPROM driver init completed.
void keyboard_pre_init_kb(void) {
    // Format the EEPROM *before* USB connects. On first boot (or after a full
    // flash wipe) eeconfig_init() erases the wear-leveling backing flash sector,
    // which stalls the CPU for the duration of the erase. If that stall happens
    // after usbConnectBus() (i.e. inside keyboard_init's eeconfig_init), the
    // host's GET_DESCRIPTOR times out and enumeration never reaches SELECTED —
    // the device parks in SUSPENDED. Doing the format here, while the device is
    // still invisible to the host, keeps the enumeration window stall-free.
    if (!eeconfig_is_enabled()) {
        he_config_set_defaults(); // persist real defaults, not zeroed BSS
        eeconfig_init();
    }

    keyboard_pre_init_user();
}

void matrix_init_custom(void) {
    // Configure the 4 address lines and 5 enable lines as push-pull outputs.
    for (uint8_t i = 0; i < HE_ADDR_LINES; i++) {
        gpio_set_pin_output(mux_addr_pins[i]);
        gpio_write_pin_low(mux_addr_pins[i]);
    }
    for (uint8_t i = 0; i < HE_MUX_COUNT; i++) {
        gpio_set_pin_output(mux_ce_pins[i]);
        gpio_write_pin_high(mux_ce_pins[i]); // all disabled at rest
    }

    // Encoder push sense line: PB15 = input pull-down, PB12 = output (idle LOW).
    gpio_set_pin_input_low(HE_ENC_PUSH_SENSE_PIN);
    gpio_set_pin_output(HE_ENC_PUSH_DRIVE_PIN);
    gpio_write_pin_low(HE_ENC_PUSH_DRIVE_PIN);

    // Default per-key config (recovered: actuation 50, release 30).
    // raw_min/raw_max are filled by calibration in the original firmware;
    // travel/peak/valley seed the rapid-trigger state machine.
    he_config_set_defaults();
}

// First boot (or EEPROM reset): persist the default per-key thresholds.
// eeconfig_init_quantum() runs eeconfig_init_kb_datablock() before this (which
// stamps the version and zeroes the block), so we only write our defaults.
void eeconfig_init_kb(void) {
    he_save_to_eeprom();
    eeconfig_init_user();
}

// Runs last in keyboard_init(), after eeconfig is guaranteed valid — load the
// persisted thresholds into RAM.
void keyboard_post_init_kb(void) {
    he_load_from_eeprom();
    keyboard_post_init_user();
}

// Physical (pre-SOCD) press state per key, plus key-cancel winner tracking
// (0 = first pair index wins, 1 = second, 0xFF = none). he_compute_pressed()
// owns he_phys_pressed[] via pointer; SOCD resolution below derives the
// reported state in he_config[].pressed from it without mutating
// he_phys_pressed[], which is what keeps the hysteresis stable.
static bool    he_phys_pressed[HE_SENSOR_COUNT];

// Debounced (stable) physical state, derived from he_phys_pressed[] after a
// symmetric N-sample confirm (HE_DEBOUNCE). SOCD resolution and the matrix
// output commit read this stable state, so a noisy sensor cannot flicker the
// reported key on either the press or release edge.
static bool    he_debounced[HE_SENSOR_COUNT];

static uint8_t socd_winner[SOCD_PAIR_COUNT] = {0xFF, 0xFF};

// Monotonic scan counter; drives the boot auto-calibration rest-sampling window
// (see HE_ADC_REST_SAMPLE_SCANS below in matrix_scan_custom()).
static uint32_t he_scan_count = 0;

bool matrix_scan_custom(matrix_row_t current_matrix[]) {
    bool changed = false;
    he_scan_count++;

    // Rising-edge flags for key-cancel (SOCD) resolution.
    bool newly_pressed[HE_SENSOR_COUNT] = {false};

    // 1) Sample every sensor and compute its *physical* press state (pre-SOCD).
    //    This drives the rapid-trigger / hysteresis state; SOCD resolution
    //    below never mutates he_phys_pressed[].
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        const he_sensor_t *s = &he_sensors[i];
        he_key_config_t   *c = &he_config[i];

        // Previous *debounced* state, to detect the rising edge after the
        // debounce stage (rather than the instantaneous state).
        bool was_debounced = he_debounced[i];

        // Encoder push (sensor 68) is not on the analog mux — read it via the
        // PB12/PB15 GPIO pair instead (see he_read_encoder_push). Its mux/addr
        // bytes in the table are dummy; skipping the analog read also keeps the
        // shared (mux 0, addr 0) channel free for key [1,2].
        if (i == HE_ENC_PUSH_INDEX) {
            he_phys_pressed[i] = he_read_encoder_push();
        } else {
            uint16_t raw = he_read_sensor_raw(s);

            // Boot auto-calibration: while the board is at rest (first N scans),
            // track each sensor's resting floor. Press drives the ADC *up* from
            // mid-scale, so the floor is the minimum raw value observed.
            if (he_scan_count <= HE_ADC_REST_SAMPLE_SCANS && raw < he_rest_floor[i]) {
                he_rest_floor[i] = raw;
            }

            // Accumulate the per-sensor noise floor / ceiling while calibrating.
            if (he_calibrating) {
                if (raw < c->raw_min) c->raw_min = raw;
                if (raw > c->raw_max) c->raw_max = raw;
            }

#if defined(RGBLIGHT_ENABLE)
            // Per-key calibration feedback (recovered): the strip starts red and a
            // key's LED latches green once it is pressed, so the user can see which
            // keys have already been calibrated. `he_rest_floor[i]` is the boot
            // auto-cal resting value, so this is robust against the still-growing
            // raw_min/raw_max during the run.
            if (he_calibrating && !he_cal_latched[i] && raw > he_rest_floor[i] + HE_CAL_PRESS_MARGIN) {
                he_cal_latched[i] = true;
                rgblight_sethsv_at(HSV_GREEN, he_led_remap[i]);
            }
#endif

            uint8_t travel = he_raw_to_travel(c, raw);
            he_compute_pressed(c, &he_phys_pressed[i], travel);
        }

        // Symmetric N-sample debounce on the instantaneous physical state. The
        // stable state only flips after HE_DEBOUNCE consecutive scans agree,
        // filtering sensor/ADC noise on *both* the press and release edges (the
        // original firmware debounced press only, so a noisy release flickered).
        if (he_phys_pressed[i] != he_debounced[i]) {
            if (++c->debounce >= HE_DEBOUNCE) {
                he_debounced[i] = he_phys_pressed[i];
                c->debounce     = 0;
            }
        } else {
            c->debounce = 0;
        }

        newly_pressed[i] = he_debounced[i] && !was_debounced;
    }

    // Lock in the boot auto-calibration once the rest-sampling window closes.
    // Map each sensor's measured resting floor -> ~0% travel and a full press
    // (rest + HE_ADC_TRAVEL_SPAN) -> ~100% travel.
    if (he_scan_count == HE_ADC_REST_SAMPLE_SCANS) {
        for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
            if (i == HE_ENC_PUSH_INDEX) {
                continue; // encoder push is a GPIO switch, not analog
            }
            uint16_t base = (he_rest_floor[i] > HE_ADC_REST_MARGIN) ? (he_rest_floor[i] - HE_ADC_REST_MARGIN) : 0;
            he_config[i].raw_min = base;
            he_config[i].raw_max = base + HE_ADC_TRAVEL_SPAN;
        }
    }

    // 2) Key-cancel (SOCD) resolution — "last input wins". The most recently
    //    pressed key in a pair owns the output; its partner is suppressed
    //    (reported released) while both are held, and only takes over once the
    //    winner physically releases.
    bool suppressed[HE_SENSOR_COUNT] = {false};

    if (he_mode == HE_MODE_KEY_CANCEL) {
        for (uint8_t p = 0; p < SOCD_PAIR_COUNT; p++) {
            uint8_t ia = socd_pairs[p][0];
            uint8_t ib = socd_pairs[p][1];

            if (newly_pressed[ia] && newly_pressed[ib]) {
                // Simultaneous press: deterministic tie-break to the key with
                // the greater travel (the more "committed" press).
                socd_winner[p] = (he_config[ia].travel >= he_config[ib].travel) ? 0 : 1;
            } else if (newly_pressed[ia]) {
                socd_winner[p] = 0;
            } else if (newly_pressed[ib]) {
                socd_winner[p] = 1;
            }

            if (!he_debounced[ia] && !he_debounced[ib]) {
                socd_winner[p] = 0xFF;
            }

            if (he_debounced[ia] && he_debounced[ib]) {
                // Suppress the loser. A 0xFF winner (cold start — both keys
                // already held with no rising edge, e.g. key-cancel mode was
                // entered mid-hold) deterministically keeps the first key.
                if (socd_winner[p] == 1) {
                    suppressed[ia] = true;
                } else {
                    suppressed[ib] = true;
                }
            }
        }
    }

    // 3) Commit the reported (post-SOCD) state to the matrix.
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        const he_sensor_t *s = &he_sensors[i];
        bool pressed         = he_debounced[i] && !suppressed[i];

        if (pressed != he_config[i].pressed) {
            he_config[i].pressed = pressed;
            changed              = true;
        }

        if (pressed) {
            current_matrix[s->row] |= ((matrix_row_t)1 << s->col);
        } else {
            current_matrix[s->row] &= ~((matrix_row_t)1 << s->col);
        }
    }

    // VIA auto-save fallback: persist any pending slider change after 2 s of
    // inactivity, in case VIA never sends the explicit save command.
    he_via_task();

    return changed;
}
