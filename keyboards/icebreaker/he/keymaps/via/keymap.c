/* Copyright 2024 Serene Industries
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include QMK_KEYBOARD_H
#include "he_matrix.h"

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        KC_ESC,  KC_1,    KC_2,    KC_3,    KC_4,    KC_5,    KC_6,    KC_7,    KC_8,    KC_9,    KC_0,    KC_MINS, KC_EQL,  KC_BSLS, KC_DEL,  KC_GRV,
        KC_TAB,  KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,    KC_Y,    KC_U,    KC_I,    KC_O,    KC_P,    KC_LBRC, KC_RBRC, KC_BSPC, KC_HOME,
        KC_CAPS, KC_A,    KC_S,    KC_D,    KC_F,    KC_G,    KC_H,    KC_J,    KC_K,    KC_L,    KC_SCLN, KC_QUOT, KC_ENT,           KC_END,
        KC_LSFT, KC_Z,    KC_X,    KC_C,    KC_V,    KC_B,    KC_N,    KC_M,    KC_COMM, KC_DOT,  KC_SLSH, KC_RSFT, KC_UP,   MO(1),
        KC_LCTL, KC_LGUI, KC_LALT,                   KC_SPC,                             KC_RALT, KC_RCTL, KC_LEFT, KC_DOWN, KC_RGHT, KC_MPLY
    ),

    [1] = LAYOUT(
        KC_ESC,  KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,   KC_F6,   KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,  KC_F12,  _______, _______, QK_UNDERGLOW_TOGGLE,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, QK_UNDERGLOW_VALUE_UP,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, KC_MPLY, QK_UNDERGLOW_VALUE_DOWN,
        MO(2),   _______, _______, _______, _______, _______, _______, QK_UNDERGLOW_HUE_DOWN, QK_UNDERGLOW_HUE_UP, QK_UNDERGLOW_SATURATION_DOWN, QK_UNDERGLOW_SATURATION_UP, KC_MUTE, KC_VOLU, _______,
        _______, _______, _______,                   _______,                            _______, _______, KC_MPRV, KC_VOLD, KC_MNXT, _______
    ),

    [2] = LAYOUT(
        QK_BOOT, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, QK_KB_0, QK_KB_1, QK_KB_2, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______,
        _______, QK_KB_4, QK_KB_8, QK_KB_3, QK_KB_5, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______,                   _______,                            _______, _______, _______, _______, _______, _______
    ),

    [3] = LAYOUT(
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        _______, _______, _______,                   _______,                            _______, _______, _______, _______, _______, _______
    ),
};

#if defined(ENCODER_MAP_ENABLE)
// Rotary encoder: clockwise = volume up, counter-clockwise = volume down.
// Rotations are only mapped on layer 0 (matches the original firmware); VIA
// can remap these per-layer via the encoder map.
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [0] = { ENCODER_CCW_CW(KC_VOLD, KC_VOLU) },
    [1] = { ENCODER_CCW_CW(_______, _______) },
    [2] = { ENCODER_CCW_CW(_______, _______) },
    [3] = { ENCODER_CCW_CW(_______, _______) },
};
#endif

// Recovered custom keycodes: delegate to the keyboard-level handler in
// he_matrix.c (QK_KB_0..QK_KB_8 -> mode/logging selection, see he_matrix.h).
bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    return he_handle_keycode(keycode, record);
}
