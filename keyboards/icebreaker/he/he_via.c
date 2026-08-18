/* Copyright 2024 Serene Industries (recovered pin map)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Icebreaker HE — VIA custom value handler.
 *
 * Implements via_custom_value_command_kb() for the recovered Hall-effect
 * custom value IDs (see he_via.h).
 *
 * Faithful behaviours reproduced from the original:
 *   - Actuation/release thresholds are *staged* on SET (IDs 1/2) and committed
 *     to all 69 sensors + EEPROM on the "save" button (ID 3).
 *   - "Set all" (ID 12) applies immediately and auto-saves, with
 *     release = actuation - 1.
 *
 * Divergences from the original (bug fixes):
 *   - Actuation mode (ID 6) and rapid-trigger tuning (IDs 7/8/9) are now
 *     persisted to EEPROM instead of being RAM-only (they reset on reboot).
 *   - The generic VIA save command (id_custom_save / 0x09), a no-op in the
 *     original, now commits staged thresholds and persists mode + tuning.
 */

#include "he_via.h"
#include "he_matrix.h"
#include "print.h"
#include "timer.h"
#include "via.h"

// Staged thresholds (recovered: SET stages, ID 3 commits).
static uint8_t staged_actuation = HE_ACTUATION_DEFAULT;
static uint8_t staged_release   = HE_RELEASE_DEFAULT;
// SET (IDs 1/2) marks the staged values dirty; the save action only applies
// them if dirty. This prevents the generic "Save" (0x09) — e.g. after a
// mode/tuning-only change — from resetting the per-key thresholds to the stale
// staged defaults (50/30).
static bool    staged_dirty     = false;

// Auto-save fallback: VIA normally persists via the "save" button (ID 3 / 0x09),
// but the original also re-saved 2 s after the last slider move. Track a
// pending flag + timestamp here; he_via_task() (called from the matrix scan)
// commits once the window elapses.
static bool     autosave_pending = false;
static uint16_t autosave_timer   = 0;

static void he_via_arm_autosave(void) {
    autosave_pending = true;
    autosave_timer   = timer_read();
}

uint8_t he_via_get_mode(void) {
    return (uint8_t)he_get_mode();
}

// Commit the staged thresholds (if any) and persist the current state — per-key
// thresholds plus the actuation mode / rapid-trigger tuning — to EEPROM.
static void he_via_commit_thresholds(void) {
    if (staged_dirty) {
        he_set_actuation(staged_actuation);
        he_set_release(staged_release);
        staged_dirty = false;

        for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
            uprintf("saving eeprom for sensor %d from via %d to eeprom %d\n", i, staged_actuation, staged_release);
        }
    }

    he_save_to_eeprom();
    uprintf("Actuation Settings Saved!\n");
}

// Housekeeping: auto-persist staged slider changes after HE_VIA_AUTOSAVE_MS of
// inactivity. Called every matrix scan from matrix_scan_custom().
void he_via_task(void) {
    if (autosave_pending && timer_elapsed(autosave_timer) >= HE_VIA_AUTOSAVE_MS) {
        autosave_pending = false;
        he_via_commit_thresholds();
    }
}

static void he_via_set_value(uint8_t *data) {
    // data = [ value_id, value_data ]
    uint8_t value_id = data[0];
    uint8_t value    = data[1];

    switch (value_id) {
        case he_id_actuation:
            staged_actuation = value;
            staged_dirty     = true;
            he_via_arm_autosave();
            break;
        case he_id_release:
            staged_release = value;
            staged_dirty   = true;
            he_via_arm_autosave();
            break;
        case he_id_save:
            he_via_commit_thresholds();
            break;
        case he_id_mode:
            he_set_mode((he_actuation_mode_t)value);
            uprintf("Actuation mode toggled! mode: %d\n", value);
            break;
        case he_id_deadzone:
            he_set_deadzone(value);
            he_via_arm_autosave();
            break;
        case he_id_engage:
            he_set_engage(value);
            he_via_arm_autosave();
            break;
        case he_id_release_dist:
            he_set_release_dist(value);
            he_via_arm_autosave();
            break;
        case he_id_set_all:
            staged_actuation = value;
            staged_release   = (value > 0) ? value - 1 : 0; // recovered: release = actuation - 1
            staged_dirty     = true;
            he_via_commit_thresholds();
            uprintf("Sensor 0..%d thresholds set. Actuation: %d, Release: %d\n", HE_SENSOR_COUNT - 1, staged_actuation, staged_release);
            break;
        case he_id_cal_start:
            he_start_calibration();
            break;
        case he_id_cal_end:
            he_end_calibration();
            break;
        default:
            break;
    }
}

static void he_via_get_value(uint8_t *data) {
    // data = [ value_id, value_data ]
    uint8_t value_id = data[0];

    switch (value_id) {
        case he_id_actuation:
            data[1] = he_get_config(0) ? he_get_config(0)->actuation : 0;
            break;
        case he_id_release:
            data[1] = he_get_config(0) ? he_get_config(0)->release : 0;
            break;
        case he_id_mode:
            data[1] = (uint8_t)he_get_mode();
            break;
        case he_id_deadzone:
            data[1] = he_get_deadzone();
            break;
        case he_id_engage:
            data[1] = he_get_engage();
            break;
        case he_id_release_dist:
            data[1] = he_get_release_dist();
            break;
        default:
            data[1] = 0;
            break;
    }
}

void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    uint8_t *command_id        = &data[0];
    uint8_t *channel_id        = &data[1];
    uint8_t *value_id_and_data = &data[2];

    (void)length;

    if (*channel_id != id_custom_channel) {
        *command_id = id_unhandled;
        return;
    }

    switch (*command_id) {
        case id_custom_set_value:
            he_via_set_value(value_id_and_data);
            break;
        case id_custom_get_value:
            he_via_get_value(value_id_and_data);
            break;
        case id_custom_save:
            // The original had no handler here, so the standard VIA "Save"
            // button did nothing. Commit staged thresholds and persist
            // thresholds + mode + tuning.
            he_via_commit_thresholds();
            break;
        default:
            *command_id = id_unhandled;
            break;
    }
}
