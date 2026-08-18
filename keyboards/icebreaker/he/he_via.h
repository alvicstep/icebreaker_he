/* Copyright 2024 Serene Industries (recovered pin map)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Icebreaker HE — VIA custom value handler.
 *
 * Handles the recovered custom value IDs on channel 0:
 *   1 = actuation threshold, 2 = release threshold, 3 = save thresholds,
 *   4 = start calibration, 5 = end calibration, 6 = actuation mode,
 *   7 = deadzone, 8 = engage distance, 9 = release distance, 12 = set all.
 */

#pragma once

#include <stdint.h>

#include "he_matrix.h"

// Recovered custom value IDs (channel 0).
enum he_via_value_id {
    he_id_actuation    = 1,
    he_id_release      = 2,
    he_id_save         = 3,
    he_id_cal_start    = 4,
    he_id_cal_end      = 5,
    he_id_mode         = 6,
    he_id_deadzone     = 7,
    he_id_engage       = 8,
    he_id_release_dist = 9,
    he_id_set_all      = 12,
};

// Exposed so keymap code can inspect the mode / config if needed.
uint8_t he_via_get_mode(void);
