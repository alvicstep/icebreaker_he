/* Copyright 2024 Serene Industries (recovered pin map)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Icebreaker HE — recovered hardware pin map.
 *
 * All constants below were recovered from the original firmware by
 * disassembling a DFU flash dump. They describe a 5x16 analog (Hall effect)
 * matrix built from five 74HC4067 16:1 multiplexers wired-OR into one ADC input.
 */

#pragma once

/* --------------------------------------------------------------------------
 * Bootloader entry (diagnostic)
 * ------------------------------------------------------------------------ */

// The STM32 ROM DFU bootloader is entered when enter_bootloader_mode_if_requested()
// (called from early_hardware_init_pre) finds a 0xDEADBEEF marker in the top
// word of SRAM (__ram0_end__ - 4). While debugging a "boots straight into DFU"
// issue, disable this early jump so the firmware boots far enough to print the
// marker value over the console. Bring-up is complete, so the early jump is
// restored — QK_BOOT (layer 2, top-left) now drops into DFU.
#define EARLY_INIT_PERFORM_BOOTLOADER_JUMP TRUE

/* --------------------------------------------------------------------------
 * Analog matrix topology
 * ------------------------------------------------------------------------ */

// Number of mux chips / CE (chip-enable) lines and address (S0-S3) lines.
#define HE_MUX_COUNT      5
#define HE_ADDR_LINES     4

// Single ADC input used for all 69 sensors: PA3 == ADC1_IN3.
#define HE_ADC_PIN        A3

// Settle delay (µs) after selecting a mux channel before the ADC samples. The
// 74HC4067 (~80 Ω Ron) + wired-OR bus stray capacitance form a small RC, so a
// short delay removes cross-channel bleed between consecutive reads.
#define HE_MUX_SETTLE_US 5

// Mux enable lines (CE, active-LOW), index 0..4.
#define HE_MUX_CE_PINS \
    { B0, A7, A6, A5, A4 }

// Mux address lines S0..S3 (4-bit channel select).
// S0=PB3, S1=PB4, S2=PB6, S3=PB5  (recovered order).
#define HE_MUX_ADDR_PINS \
    { B3, B4, B6, B5 }

// Total sensor count (68 Hall keys + 1 rotary-encoder push at index 68).
#define HE_SENSOR_COUNT 69

/* --------------------------------------------------------------------------
 * Matrix dimensions exposed to QMK (custom matrix: 5 rows x 16 cols).
 * ------------------------------------------------------------------------ */

#define MATRIX_ROWS 5
#define MATRIX_COLS 16

/* --------------------------------------------------------------------------
 * Hall-effect tuning defaults (recovered from matrix_init @ 0x080096F8).
 * ------------------------------------------------------------------------ */

#define HE_ACTUATION_DEFAULT    50   // % travel that registers a press
#define HE_RELEASE_DEFAULT      30   // % travel that releases the key
// Hardcoded rapid-trigger tuning defaults: deadzone 50, engage 15, disengage
// (release distance) 10. The boot actuation mode is set in he_matrix.c.
#define HE_DEADZONE_DEFAULT     50
#define HE_ENGAGE_DEFAULT       15
#define HE_RELEASE_DIST_DEFAULT 10

/* VIA slider ranges (from icebreaker_HE_via_definitions.json). The matrix
 * setters clamp raw-HID values to these bounds so a malformed / out-of-range
 * packet cannot store an unusable threshold (e.g. actuation 0 or 255) into
 * RAM or EEPROM. */
#define HE_ACTUATION_MIN        10
#define HE_ACTUATION_MAX        90
#define HE_RELEASE_MIN          10
#define HE_RELEASE_MAX          90
#define HE_DEADZONE_MIN         15
#define HE_DEADZONE_MAX         60
#define HE_ENGAGE_MIN            5
#define HE_ENGAGE_MAX           20
#define HE_RELEASE_DIST_MIN      5
#define HE_RELEASE_DIST_MAX     20

/* ADC is read at 12-bit resolution in the original firmware. */
#define HE_ADC_RESOLUTION_BITS  12
#define HE_ADC_RAW_MAX          ((1 << HE_ADC_RESOLUTION_BITS) - 1)

/* The Hall sensors are bipolar: at rest (magnet far away) they sit at
 * mid-scale (VCC/2), and pressing a key drives the output toward VCC
 * (full scale). This is the uncalibrated "rest" reference used before the
 * VIA calibration runs. */
#define HE_ADC_RAW_MID          ((HE_ADC_RAW_MAX + 1) / 2)

/* --------------------------------------------------------------------------
 * Boot-time auto-calibration
 *
 * The live board confirmed the press polarity: at rest the Hall sensors sit
 * near mid-scale (~2150 raw) and pressing a key drives the ADC *up*. A full
 * bottom-out measured ~2842 raw — a swing of ~700 counts, NOT the full 4095
 * scale the uncalibrated defaults assumed. Because the resting value varies
 * slightly per sensor, the firmware samples each sensor's resting floor over
 * the first N scans after power-up and maps:
 *
 *     rest        -> ~0% travel    (raw_min = rest_floor - margin)
 *     full press  -> ~100% travel  (raw_max = raw_min + HE_ADC_TRAVEL_SPAN)
 *
 * These are conservative starting points; the VIA calibration (start/end
 * buttons) still overrides them with true per-key min/max if desired.
 * ------------------------------------------------------------------------ */

// Number of full matrix scans to sample the resting value before locking it in.
#define HE_ADC_REST_SAMPLE_SCANS 64

// Raw counts subtracted from the measured rest floor so a resting key reads
// slightly above 0% travel (absorbs ADC noise) instead of exactly 0%.
#define HE_ADC_REST_MARGIN 20

// Full-press swing in raw ADC counts (measured ~692 on the live board). This
// is the span mapped to 0..100% travel after auto-calibration.
#define HE_ADC_TRAVEL_SPAN 700

// Minimum plausible raw span (raw_max - raw_min) for a calibrated key. Below
// this the calibration is treated as "low ceiling" (the key was never actually
// pressed). A real full press measures ~700 counts and ADC noise is <100, so
// 200 rejects noise-only calibrations.
#define HE_ADC_MIN_SPAN 200

// During VIA calibration, a key counts as "pressed" (and its LED latches green)
// once its raw reading climbs this far above the measured rest floor. Half the
// full-press swing (~350 counts) cleanly separates a press from ADC noise while
// still catching a light press.
#define HE_CAL_PRESS_MARGIN (HE_ADC_TRAVEL_SPAN / 2)

/* QMK's analog driver defaults to 10-bit (ADC_CFGR1_RES_10BIT); match the
 * original firmware's 12-bit ADC group configuration. */
#define ADC_RESOLUTION ADC_CFGR1_RES_12BIT

/* --------------------------------------------------------------------------
 * Encoder (recovered from disassembly @ flash 0x0800E4xx / 0x08009E18)
 *
 *   - A rotation pin = PB14  (input, pull-up)
 *   - B rotation pin = PB13  (input, pull-up)
 *   - Push button    = matrix [4,9] (sensor index 68). It is *not* on the
 *     analog mux — it is a mechanical switch read as:
 *       PB12 -> driven HIGH (sense drive line)
 *       PB15 -> input, pull-down (sense line); pressed when PB15 reads HIGH.
 * ------------------------------------------------------------------------ */

#define ENCODER_A_PINS \
    { B14 }
#define ENCODER_B_PINS \
    { B13 }
#define ENCODER_RESOLUTION 4

// Encoder push-button GPIO sense lines (see he_matrix.c).
#define HE_ENC_PUSH_DRIVE_PIN B12
#define HE_ENC_PUSH_SENSE_PIN B15

// Symmetric per-key debounce: a press or release is only reported after this
// many consecutive matrix scans agree. Applied to every key (analog Hall keys
// *and* the GPIO encoder push) so a noisy sensor cannot flicker the output.
#define HE_DEBOUNCE 5

/* --------------------------------------------------------------------------
 * EEPROM (wear-leveled embedded flash)
 * ------------------------------------------------------------------------ */

// STM32F411 has no dedicated EEPROM. QMK's default wear-leveling driver maps
// the *last* 128 KB flash sector as backing store (whole-sector granularity).
// 16 KB backing -> 8 KB logical EEPROM, leaving room for VIA's dynamic keymap
// *and* the 142-byte per-key + settings Hall config below.
#define WEAR_LEVELING_BACKING_SIZE 16384

// Per-key Hall config: 69 sensors x 2 bytes (see he_eeprom_key_config_t in
// he_matrix.h) followed by a 4-byte settings record (actuation mode +
// rapid-trigger tuning, see he_settings_eeprom_t). Stored in QMK's keyboard
// data block, which lives between the core eeconfig and the VIA region;
// EECONFIG_SIZE grows automatically, pushing the VIA dynamic keymap up so the
// two never overlap.
#define EECONFIG_KB_DATA_SIZE (HE_SENSOR_COUNT * 2 + 4)
// Bumped to 3 when the per-key record shrank from 6 -> 2 bytes (dropping the
// never-read `reserved`/`engage`/`raw` recovered fields). The version lives in
// EECONFIG_KEYBOARD; a mismatch invalidates the block so it is re-seeded.
#define EECONFIG_KB_DATA_VERSION 3

// VIA auto-save fallback: persist a slider change this many ms after the last
// SET if VIA never sends the explicit save command (see he_via.c he_via_task).
#define HE_VIA_AUTOSAVE_MS 2000

/* --------------------------------------------------------------------------
 * RGB lighting — WS2812/SK6812 strip driven by PWM + DMA (recovered from the
 * original firmware; the vendor used rgblight, not rgb_matrix).
 *
 *   data pin : PA8 == TIM1_CH1 (AF1), push-pull, very-high speed
 *   timer    : TIM1 (advanced timer, main CH1 output — no complementary pin)
 *   DMA      : DMA2 Stream 5 / Channel 6 (TIM1_UP update event)
 *   LEDs     : 68 (one per Hall key; the encoder push has no LED)
 *   order    : GRB (also the QMK ws2812 default)
 *
 * QMK's ws2812_pwm.c maps 1:1 onto this hardware. The LED count and the
 * rgblight feature are declared in keyboard.json; these low-level PWM/DMA
 * selectors have no data-driven equivalent and live here.
 * ------------------------------------------------------------------------ */

// PA8 = TIM1_CH1, alternate function 1 (push-pull output mode).
#define WS2812_PWM_DRIVER        PWMD1
#define WS2812_PWM_CHANNEL       1
#define WS2812_PWM_PAL_MODE      1

// TIM1 update event is routed to DMA2 Stream 5 / Channel 6 on STM32F4.
#define WS2812_PWM_DMA_STREAM    STM32_DMA2_STREAM5
#define WS2812_PWM_DMA_CHANNEL   6

// The recovered firmware encodes a "1" bit as 0x26 (38 ticks = 792 ns) and a
// "0" bit as 0x10 (16 ticks = 333 ns) at a 48 MHz tick. QMK's WS2812B default
// (T1H = 900 ns -> 43 ticks) shortens the "1" bit's low phase below what the
// strip can reliably latch, corrupting the stream and making it blink. Match
// the vendor's T1H so the timing is bit-for-bit identical to stock.
#define WS2812_T1H               792

// rgblight boots ON in static mode. Colour/saturation/brightness are the exact
// values set in VIA (hue 114 = teal/cyan, sat 128, val 128) so a fresh flash or
// EEPROM reset reproduces the configured look instead of the QMK defaults.
#define RGBLIGHT_DEFAULT_MODE    RGBLIGHT_MODE_STATIC_LIGHT
#define RGBLIGHT_DEFAULT_HUE     114
#define RGBLIGHT_DEFAULT_SAT     128

// 68 LEDs at full brightness draw ~4 A — far past USB's 0.5 A — which browns
// the board out on reconnect (blinding white flash, then a reset loop that
// leaves the strip red). Cap the maximum brightness (including VIA) and boot
// at the same level. This is a *separate* fix from WS2812_T1H below, which
// addresses steady-state blinking from data corruption.
#define RGBLIGHT_LIMIT_VAL       128
#define RGBLIGHT_DEFAULT_VAL     128
