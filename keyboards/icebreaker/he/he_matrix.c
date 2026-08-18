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
 *   - EEPROM persistence of the per-key thresholds (6-byte structs, 414 B) and
 *     the global settings (actuation mode + rapid-trigger tuning, 4 B).
 *   - 12-bit ADC group config (ADC_RESOLUTION in config.h).
 *
 * Encoder (recovered):
 *   - A/B rotation = PB14/PB13 (standard QMK encoder.c, see config.h).
 *   - Push button  = matrix [4,9], read via PB12 (drive HIGH) + PB15 (sense,
 *     pull-down). Implemented below (sensor index 68).
 */

#include "he_matrix.h"

#include "analog.h"
#include "boot_trace.h"
#include "eeconfig.h"
#include "gpio.h"
#include "keyboard.h"
#include "print.h"
#include "timer.h"
#include "usb_main.h"

/* --------------------------------------------------------------------------
 * Boot diagnostics
 * ------------------------------------------------------------------------ */

// The QMK DFU bootloader stores a 0xDEADBEEF marker at the top word of SRAM
// (__ram0_end__ - 4) to signal "jump to bootloader" after a soft reset. Reading
// it lets us see whether a previous session left the marker set (which would
// make enter_bootloader_mode_if_requested() bounce us straight back into DFU).
extern uint8_t __ram0_end__;
static uint32_t he_bootloader_magic(void) {
    return *(volatile uint32_t *)((uint32_t)&__ram0_end__ - 4);
}

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
static he_actuation_mode_t he_mode = HE_MODE_NORMAL;
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

// Rotary-encoder push is the last sensor-table entry (68), read via GPIO
// rather than the analog mux.
#define HE_ENC_PUSH_INDEX (HE_SENSOR_COUNT - 1)

he_key_config_t *he_get_config(uint8_t index) {
    return (index < HE_SENSOR_COUNT) ? &he_config[index] : NULL;
}

void he_set_actuation(uint8_t value) {
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        he_config[i].actuation = value;
    }
}

void he_set_release(uint8_t value) {
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
    he_tuning.deadzone = value;
}

uint8_t he_get_engage(void) {
    return he_tuning.engage;
}

void he_set_engage(uint8_t value) {
    he_tuning.engage = value;
}

uint8_t he_get_release_dist(void) {
    return he_tuning.release_dist;
}

void he_set_release_dist(uint8_t value) {
    he_tuning.release_dist = value;
}

/* --------------------------------------------------------------------------
 * Recovered custom keycodes (QK_KB_0 .. QK_KB_8)
 *
 * The original process_record handler (flash @ 0x0800abe4) subtracts QK_KB
 * (0x7E00) from the keycode and switches on the result (0..8), acting on key
 * press only and returning false (consumed) for every QK_KB keycode:
 *
 *   QK_KB_0 -> APC mode (actuation = 85)   QK_KB_3 -> logging 0
 *   QK_KB_1 -> RT mode  (actuation = 0)    QK_KB_4 -> logging 1
 *   QK_KB_2 -> Key Cancel (actuation = 170) QK_KB_5 -> logging 2 (blocking)
 *                                          QK_KB_6 -> logging 3 (none)
 *                                          QK_KB_7 -> logging 4
 *                                          QK_KB_8 -> logging 5
 *
 * The original also persists a mode-specific default threshold (85/0/170) to
 * EEPROM via a helper @ 0x0800ab90; that side effect is omitted here in favour
 * of the he_set_mode() path used by the VIA handler (which persists the mode
 * itself).
 * ------------------------------------------------------------------------ */

// Diagnostic logging level (set by QK_KB_3..8). The original gates trace output
// on this value; the full logging subsystem is not reconstructed, so we keep
// only the recovered selector for API completeness.
static uint8_t he_logging_mode;

uint8_t he_get_logging_mode(void) {
    return he_logging_mode;
}

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
        case QK_KB_3:
            if (record->event.pressed) {
                he_logging_mode = 0;
                uprintf("Logging Mode set to 0\n");
            }
            return false;
        case QK_KB_4:
            if (record->event.pressed) {
                he_logging_mode = 1;
                uprintf("Logging Mode set to 1\n");
            }
            return false;
        case QK_KB_5:
            if (record->event.pressed) {
                he_logging_mode = 2;
                uprintf("Logging Mode set to 2 (blocking keystrokes)\n");
            }
            return false;
        case QK_KB_6:
            if (record->event.pressed) {
                he_logging_mode = 3;
                uprintf("Logging Mode set to 3(none)\n");
            }
            return false;
        case QK_KB_7:
            if (record->event.pressed) {
                he_logging_mode = 4;
                uprintf("Logging Mode set to 4\n");
            }
            return false;
        case QK_KB_8:
            if (record->event.pressed) {
                he_logging_mode = 5;
                uprintf("Logging Mode set to 5\n");
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

void he_start_calibration(void) {
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        if (i == HE_ENC_PUSH_INDEX) {
            continue; // encoder push is a GPIO switch, not analog
        }
        he_config[i].raw_min = HE_ADC_RAW_MAX;
        he_config[i].raw_max = 0;
    }
    he_calibrating = true;
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
    uprintf("Calibration ended.\n");
}

bool he_is_calibrating(void) {
    return he_calibrating;
}

/* --------------------------------------------------------------------------
 * EEPROM persistence (QMK keyboard data block)
 *
 * Layout: 69 x he_eeprom_key_config_t (414 B) followed by one
 * he_settings_eeprom_t (4 B) = 418 B total. The per-key records store the
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
        he_config[i].actuation = rec[i].actuation;
        he_config[i].release   = rec[i].release;
    }

    he_settings_eeprom_t settings;
    eeconfig_read_kb_datablock(&settings, HE_SETTINGS_EEPROM_OFFSET, sizeof(settings));
    if (settings.actuation_mode <= HE_MODE_KEY_CANCEL) {
        he_mode = (he_actuation_mode_t)settings.actuation_mode;
    }
    he_tuning.deadzone     = settings.deadzone;
    he_tuning.engage       = settings.engage;
    he_tuning.release_dist = settings.release_dist;
}

void he_save_to_eeprom(void) {
    he_eeprom_key_config_t rec[HE_SENSOR_COUNT];
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        rec[i].actuation = he_config[i].actuation;
        rec[i].release   = he_config[i].release;
        rec[i].reserved  = 0;
        rec[i].engage    = HE_ENGAGE_DEFAULT;
        rec[i].raw       = 0x02BC; // recovered default (700)
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
//   Rapid trigger : release when travel drops by `release_dist` from its peak
//                   (or below absolute `release`); re-press when travel rises
//                   by `engage` from its valley, once above the `deadzone`
//                   rest band (or above absolute `actuation`).
//   Key cancel    : same as Normal; SOCD pair resolution happens in the scan.
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
        // Bipolar Hall: rest = mid-scale, press = toward full scale. These are
        // only a safe fallback — the boot auto-calibration in matrix_scan_custom()
        // overwrites raw_min/raw_max with the measured resting floor + span.
        he_config[i].raw_min   = HE_ADC_RAW_MID;
        he_config[i].raw_max   = HE_ADC_RAW_MAX;
        he_config[i].travel    = 0;
        he_config[i].peak      = 0;
        he_config[i].valley    = 0;
        he_config[i].pressed   = false;
        he_rest_floor[i]       = HE_ADC_RAW_MAX;
    }
}

// Runs in keyboard_setup(), after eeprom_driver_init() + matrix_setup() and
// immediately before the USB driver comes up. A marker here proves the
// wear-leveling EEPROM driver init completed.
void keyboard_pre_init_kb(void) {
    boot_trace(BT_PRE_INIT_KB);

    // Format the EEPROM *before* USB connects. On first boot (or after a full
    // flash wipe) eeconfig_init() erases the wear-leveling backing flash sector,
    // which stalls the CPU for the duration of the erase. If that stall happens
    // after usbConnectBus() (i.e. inside keyboard_init's eeconfig_init), the
    // host's GET_DESCRIPTOR times out and enumeration never reaches SELECTED —
    // the device parks in SUSPENDED. Doing the format here, while the device is
    // still invisible to the host, keeps the enumeration window stall-free.
    if (!eeconfig_is_enabled()) {
        boot_trace(BT_EE_INIT_PRE);
        he_config_set_defaults(); // persist real defaults, not zeroed BSS
        eeconfig_init();
    }

    keyboard_pre_init_user();
}

static uint32_t read_psp(void) {
    uint32_t psp;
    __asm__ volatile("mrs %0, psp" : "=r"(psp));
    return psp;
}

void matrix_init_custom(void) {
    boot_trace(BT_MATRIX_INIT);
    boot_trace_value(14, timer_read32()); // ms timestamp at matrix init
    boot_trace_value(28, read_psp());     // process stack pointer at matrix init (overflow probe)
    uprintf("HE: matrix_init_custom() entry magic=0x%08lx\n", (unsigned long)he_bootloader_magic());

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

    uprintf("HE: matrix_init_custom() done\n");
}

// First boot (or EEPROM reset): persist the default per-key thresholds.
// eeconfig_init_quantum() runs eeconfig_init_kb_datablock() before this (which
// stamps the version and zeroes the block), so we only write our defaults.
void eeconfig_init_kb(void) {
    boot_trace(BT_EE_INIT_KB);
    uprintf("HE: eeconfig_init_kb() -- first boot / EEPROM reset\n");
    he_save_to_eeprom();
    eeconfig_init_user();
}

// Runs as the FIRST statement of keyboard_post_init_quantum() (weak override).
// If this marker is present, keyboard_init() reached its tail call to
// keyboard_post_init_quantum(); if absent, keyboard_init() stopped earlier.
void keyboard_post_init_modules(void) {
    boot_trace(BT_POST_INIT_MODULES);
    boot_trace_value(27, read_psp()); // process stack pointer at post-init (overflow probe)
}

// Runs last in keyboard_init(), after eeconfig is guaranteed valid — load the
// persisted thresholds into RAM.
void keyboard_post_init_kb(void) {
    boot_trace(BT_POST_INIT_KB);
    boot_trace_value(15, timer_read32()); // ms timestamp at post-init (erase spans 14->15)
    uprintf("HE: keyboard_post_init_kb() magic=0x%08lx mode=%d\n", (unsigned long)he_bootloader_magic(), (int)he_mode);
    he_load_from_eeprom();
    keyboard_post_init_user();
    uprintf("HE: keyboard_post_init_kb() done\n");
}

// Debounce state for the encoder push button (sensor 68). Mirrors the
// original's N-sample confirm (5 consecutive equal reads).
static uint8_t he_enc_push_debounce = 0;

// Physical (pre-SOCD) press state per key, plus key-cancel winner tracking
// (0 = first pair index wins, 1 = second, 0xFF = none). he_compute_pressed()
// owns he_phys_pressed[] via pointer; SOCD resolution below derives the
// reported state in he_config[].pressed from it without mutating
// he_phys_pressed[], which is what keeps the hysteresis stable.
static bool    he_phys_pressed[HE_SENSOR_COUNT];
static uint8_t socd_winner[SOCD_PAIR_COUNT] = {0xFF, 0xFF};

static uint32_t he_scan_count = 0;

// Flash-diagnostic state. Slots written via boot_trace_value() (see below):
//   0..2  = raw ADC of sensors 0/32/46 on the first scan (resting values)
//   3     = encoder-push GPIO level on the first scan
//   4     = he_scan_count when USB_DRIVER.state first hit USB_ACTIVE
//   5     = he_scan_count when USB_DRIVER.state first hit USB_SUSPENDED
//   6     = index of the first sensor reported pressed (blank = none)
//   7     = he_scan_count at first press
//   8     = raw ADC at first press (0xFFFF = encoder push, non-analog)
//   9/10  = max/min raw ADC swing observed up to first suspend
//   11/12 = suspend_power_down_kb() / suspend_wakeup_init_kb() call counts
//   14    = timer_read32() ms timestamp at matrix_init_custom()
//   15    = timer_read32() ms timestamp at keyboard_post_init_kb()
//           (the 14->15 gap includes the EEPROM wear-leveling flash erase)
//   16    = timer_read32() ms timestamp at the first matrix scan
//   20..24= USB event bitmask (usb_event_debug) at scans 5/20/100/500/2000
//   25/26 = USB event bitmask at scans 1/2 (captured before early suspend park)
//   27    = process stack pointer (PSP) at keyboard_post_init_modules()
//   28    = process stack pointer (PSP) at matrix_init_custom()
//           (PSP grows DOWN from 0x20000c00; < 0x20000400 => overflow)
//   29    = global max raw ADC observed by scan 5000 (press polarity probe)
//   30    = global min raw ADC observed by scan 5000 (press polarity probe)
//   31    = global max raw ADC observed by scan 20000 (late press probe)
//   32    = auto-calibrated raw_min for sensor 0 (after rest-sampling window)
//   33    = auto-calibrated raw_max for sensor 0 (raw_min + HE_ADC_TRAVEL_SPAN)
//           bit0=RESET bit1=ADDRESS bit2=CONFIGURED bit3=UNCONFIGURED
//           bit4=SUSPEND bit5=WAKEUP bit6=STALLED
static bool     he_usb_active_seen    = false;
static bool     he_usb_suspended_seen = false;
static bool     he_first_press_seen   = false;
static uint16_t he_max_raw            = 0;
static uint16_t he_min_raw            = HE_ADC_RAW_MAX;
static uint32_t he_suspend_count      = 0;
static uint32_t he_wakeup_count       = 0;

// Raw USB event accumulator. usb_event_debug() (a weak hook in usb_main.c) is
// called from the OTG interrupt as each global event arrives; we only set bits
// here (never read) and snapshot the mask from the scan loop, so it is safe
// across the ISR/thread boundary.
static volatile uint32_t he_usb_event_mask = 0;

void usb_event_debug(usbevent_t event) {
    if ((uint32_t)event < 32) {
        he_usb_event_mask |= (1UL << (uint32_t)event);
    }
}

// Weak hooks (platforms/suspend.c + quantum/quantum.c) overridden so the host
// suspend / device resume events are visible in the flash trace. This tells us
// whether the device is parking in QMK's suspend loop (and whether it ever
// wakes), independent of the USB-state markers sampled from the scan loop.
void suspend_power_down_kb(void) {
    boot_trace(BT_SUSPEND_KB);
    boot_trace_value(11, ++he_suspend_count);
    suspend_power_down_user();
}

void suspend_wakeup_init_kb(void) {
    boot_trace(BT_WAKEUP_KB);
    boot_trace_value(12, ++he_wakeup_count);
    suspend_wakeup_init_user();
}

bool matrix_scan_custom(matrix_row_t current_matrix[]) {
    bool changed = false;
    he_scan_count++;

    if (he_scan_count == 1) {
        boot_trace(BT_SCAN_FIRST);
        boot_trace_value(16, timer_read32()); // ms timestamp at first scan
    }

    // Rising-edge flags for key-cancel (SOCD) resolution.
    bool newly_pressed[HE_SENSOR_COUNT] = {false};

    // 1) Sample every sensor and compute its *physical* press state (pre-SOCD).
    //    This drives the rapid-trigger / hysteresis state; SOCD resolution
    //    below never mutates he_phys_pressed[].
    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        const he_sensor_t *s = &he_sensors[i];
        he_key_config_t   *c = &he_config[i];

        // Encoder push (sensor 68) is not on the analog mux — read it via the
        // PB12/PB15 GPIO pair instead (see he_read_encoder_push). Its mux/addr
        // bytes in the table are dummy; skipping the analog read also keeps the
        // shared (mux 0, addr 0) channel free for key [1,2].
        if (i == HE_ENC_PUSH_INDEX) {
            bool raw = he_read_encoder_push();
            if (he_scan_count == 1) {
                boot_trace_value(3, raw ? 1U : 0U);
            }
            if (raw != he_phys_pressed[i]) {
                if (++he_enc_push_debounce >= HE_ENC_PUSH_DEBOUNCE) {
                    bool was            = he_phys_pressed[i];
                    he_phys_pressed[i]  = raw;
                    he_enc_push_debounce = 0;
                    newly_pressed[i]    = raw && !was;
                }
            } else {
                he_enc_push_debounce = 0;
            }
            continue;
        }

        uint16_t raw = he_read_sensor_raw(s);

        // Diagnostic: track the global min/max raw swing and capture a few
        // reference channels on the very first scan.
        if (raw > he_max_raw) he_max_raw = raw;
        if (raw < he_min_raw) he_min_raw = raw;
        if (he_scan_count == 1) {
            if (i == 0)  boot_trace_value(0, raw);
            if (i == 32) boot_trace_value(1, raw);
            if (i == 46) boot_trace_value(2, raw);
        }

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

        uint8_t travel = he_raw_to_travel(c, raw);
        bool    was    = he_phys_pressed[i];
        he_compute_pressed(c, &he_phys_pressed[i], travel);
        newly_pressed[i] = he_phys_pressed[i] && !was;
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
        boot_trace_value(32, he_config[0].raw_min);
        boot_trace_value(33, he_config[0].raw_max);
        uprintf("HE: auto-cal sensor0 raw_min=%u raw_max=%u (span=%u)\n",
                (unsigned)he_config[0].raw_min, (unsigned)he_config[0].raw_max,
                (unsigned)HE_ADC_TRAVEL_SPAN);
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

            if (!he_phys_pressed[ia] && !he_phys_pressed[ib]) {
                socd_winner[p] = 0xFF;
            }

            if (he_phys_pressed[ia] && he_phys_pressed[ib]) {
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
        bool pressed         = he_phys_pressed[i] && !suppressed[i];

        if (pressed != he_config[i].pressed) {
            he_config[i].pressed = pressed;
            changed              = true;
        }

        if (pressed && !he_first_press_seen) {
            he_first_press_seen = true;
            boot_trace_value(6, i);            // first pressed sensor index
            boot_trace_value(7, he_scan_count); // scan count at first press
            if (i == HE_ENC_PUSH_INDEX) {
                boot_trace_value(8, 0xFFFFU);   // encoder push (non-analog)
            } else {
                boot_trace_value(8, he_read_sensor_raw(s)); // raw at first press
            }
        }

        if (pressed) {
            current_matrix[s->row] |= ((matrix_row_t)1 << s->col);
        } else {
            current_matrix[s->row] &= ~((matrix_row_t)1 << s->col);
        }
    }

    // Non-USB diagnostics: record the ChibiOS USB driver state (mapped to a
    // boot_trace marker) and a heartbeat so we can tell, after a DFU readback,
    // (a) how far USB enumeration got and (b) whether the main loop is still
    // running. boot_trace*() is write-once, so this is cheap after the first hit.
    {
        usbstate_t us = USB_DRIVER.state;
        if (us >= USB_STOP && us <= USB_SUSPENDED) {
            boot_trace(BT_USB_STOP + ((uint32_t)us - (uint32_t)USB_STOP));
        }
        if (us == USB_ACTIVE && !he_usb_active_seen) {
            he_usb_active_seen = true;
            boot_trace_value(4, he_scan_count);
        }
        if (us == USB_SUSPENDED && !he_usb_suspended_seen) {
            he_usb_suspended_seen = true;
            boot_trace_value(5, he_scan_count);
            boot_trace_value(9, he_max_raw);  // resting ADC swing up to suspend
            boot_trace_value(10, he_min_raw);
        }
    }

    // Snapshot the accumulated raw USB events at a few scan counts so the DFU
    // readback shows whether the host ever reset/addressed/configured us. The
    // scan-1/2 snapshots matter most: if the device parks in suspend early we
    // never reach the later scan counts.
    {
        uint32_t mask = he_usb_event_mask;
        if (he_scan_count == 1)    boot_trace_value(25, mask);
        if (he_scan_count == 2)    boot_trace_value(26, mask);
        if (he_scan_count == 5)    boot_trace_value(20, mask);
        if (he_scan_count == 20)   boot_trace_value(21, mask);
        if (he_scan_count == 100)  boot_trace_value(22, mask);
        if (he_scan_count == 500)  boot_trace_value(23, mask);
        if (he_scan_count == 2000) boot_trace_value(24, mask);
    }

    // Late ADC-swing capture: he_max_raw/he_min_raw accumulate every analog
    // scan, so by scan 5000 they reflect any key pressed shortly after boot.
    // This reveals the Hall press polarity (up vs. down) and magnitude even
    // though the current raw_min/raw_max default may not register a press.
    if (he_scan_count == 5000) {
        boot_trace_value(29, he_max_raw);
        boot_trace_value(30, he_min_raw);
        uprintf("HE: swing@5000 min=%u max=%u\n", (unsigned)he_min_raw, (unsigned)he_max_raw);
    } else if (he_scan_count == 20000) {
        boot_trace_value(31, he_max_raw);
        uprintf("HE: swing@20000 min=%u max=%u\n", (unsigned)he_min_raw, (unsigned)he_max_raw);
    }

    boot_trace_heartbeat(he_scan_count / 2000U);

    // Boot / ADC sanity diagnostics (throttled).
    if (he_scan_count == 1) {
        uprintf("HE: first matrix scan complete (69 sensors sampled)\n");
    } else if ((he_scan_count % 5000) == 0) {
        uint16_t r0  = he_read_sensor_raw(&he_sensors[0]);   // key [0,0]
        uint16_t r32 = he_read_sensor_raw(&he_sensors[32]);  // A (SOCD pair)
        uint16_t r46 = he_read_sensor_raw(&he_sensors[46]);  // Z (SOCD pair)
        uprintf("HE: scan=%lu adc[0]=%u adc[32]=%u adc[46]=%u\n",
                (unsigned long)he_scan_count, (unsigned)r0, (unsigned)r32, (unsigned)r46);
    }

    return changed;
}
