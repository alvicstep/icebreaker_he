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
 *   - Actuation mode (ID 6) and rapid-trigger tuning (IDs 7/8/9) are RAM-only
 *     and reset on reboot.
 *   - The generic VIA save command (id_custom_save / 0x09) has no handler in
 *     the original — thresholds are only persisted via the ID 3 button.
 */

#include "he_via.h"
#include "he_matrix.h"
#include "print.h"
#include "via.h"

// Staged thresholds (recovered: SET stages, ID 3 commits).
static uint8_t staged_actuation = HE_ACTUATION_DEFAULT;
static uint8_t staged_release   = HE_RELEASE_DEFAULT;

uint8_t he_via_get_mode(void) {
    return (uint8_t)he_get_mode();
}

// Commit the staged thresholds to every sensor and persist them to EEPROM.
static void he_via_commit_thresholds(void) {
    he_set_actuation(staged_actuation);
    he_set_release(staged_release);

    for (uint8_t i = 0; i < HE_SENSOR_COUNT; i++) {
        uprintf("saving eeprom for sensor %d from via %d to eeprom %d\n", i, staged_actuation, staged_release);
    }

    he_save_to_eeprom();
    uprintf("Actuation Settings Saved!\n");
}

static void he_via_set_value(uint8_t *data) {
    // data = [ value_id, value_data ]
    uint8_t value_id = data[0];
    uint8_t value    = data[1];

    switch (value_id) {
        case he_id_actuation:
            staged_actuation = value;
            break;
        case he_id_release:
            staged_release = value;
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
            break;
        case he_id_engage:
            he_set_engage(value);
            break;
        case he_id_release_dist:
            he_set_release_dist(value);
            break;
        case he_id_set_all:
            staged_actuation = value;
            staged_release   = (value > 0) ? value - 1 : 0; // recovered: release = actuation - 1
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
            // Intentionally a no-op: the original firmware has no handler for
            // the generic save command — thresholds persist only via ID 3.
            break;
        default:
            *command_id = id_unhandled;
            break;
    }
}
