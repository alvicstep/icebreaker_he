/* Copyright 2024 Serene Industries (recovered pin map)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Non-USB boot-stage tracing for board bring-up.
 *
 * The QMK HID console can't print until USB enumerates, which happens late in
 * the boot sequence. To localise the "board boots but never enumerates" hang,
 * each boot stage writes a 32-bit marker into a dedicated region of flash
 * (sector 5, entirely beyond the firmware image). After the board is
 * power-cycled and re-entered into DFU, that region is read back over DFU and
 * the highest marker present tells us how far boot got.
 */

#pragma once

#include <stdint.h>

// Boot-stage trace points. Each stage writes (0xAA000000 | stage) at offset
// (stage * 4) within the trace sector.
enum {
    BT_EARLY_PRE_ENTRY  = 1,  // crt0 -> early_hardware_init_pre() entry
    BT_EARLY_PRE_EXIT   = 2,  // early_hardware_init_pre() done (before gpio/clock)
    BT_EARLY_POST_ENTRY = 3,  // early_hardware_init_post() (after gpio/clock init)
    BT_EARLY_POST_EXIT  = 4,  // early_hardware_init_post() done
    BT_BOARD_INIT_ENTRY = 5,  // board_init() entry
    BT_BOARD_INIT_EXIT  = 6,  // board_init() done
    BT_PRE_INIT_KB      = 7,  // keyboard_pre_init_kb() — after eeprom_driver_init + matrix_setup, BEFORE USB
    BT_MATRIX_INIT      = 8,  // matrix_init_custom() — after USB is up
    BT_EE_INIT_KB       = 9,  // eeconfig_init_kb() (first boot / EEPROM reset)
    BT_EE_INIT_PRE      = 36, // eeconfig_init() ran in keyboard_pre_init_kb (BEFORE USB connect)
    BT_POST_INIT_KB     = 10, // keyboard_post_init_kb()
    BT_POST_INIT_MODULES = 37, // keyboard_post_init_modules() override (1st stmt of keyboard_post_init_quantum)
    BT_SCAN_FIRST       = 11, // first matrix_scan_custom() (main loop reached)

    // USB driver state sampled from the main loop. Encoded as 20 + state where
    // state is ChibiOS usbstate_t (USB_STOP=1, USB_READY=2, USB_SELECTED=3,
    // USB_ACTIVE=4, USB_SUSPENDED=5). The highest marker tells us how far the
    // USB enumeration got: STOP means the host never even sent a bus reset
    // (pull-up / connection problem), READY means a reset arrived but the
    // descriptor/config exchange stalled, ACTIVE means full enumeration.
    BT_USB_STOP         = 21, // USB_DRIVER.state == USB_STOP (1)
    BT_USB_READY        = 22, // USB_DRIVER.state == USB_READY (2)
    BT_USB_SELECTED     = 23, // USB_DRIVER.state == USB_SELECTED (3)
    BT_USB_ACTIVE       = 24, // USB_DRIVER.state == USB_ACTIVE (4)
    BT_USB_SUSPENDED    = 25, // USB_DRIVER.state == USB_SUSPENDED (5)

    // CPU fault handlers (weak in ChibiOS crt0; overridden so a crash is visible).
    BT_FAULT_HARD       = 30, // HardFault_Handler
    BT_FAULT_MEMMANAGE  = 31, // MemManage_Handler
    BT_FAULT_BUS        = 32, // BusFault_Handler
    BT_FAULT_USAGE      = 33, // UsageFault_Handler

    // Suspend / wakeup hooks (overridden in he_matrix.c).
    BT_SUSPEND_KB       = 34, // suspend_power_down_kb() — host suspended the bus
    BT_WAKEUP_KB        = 35, // suspend_wakeup_init_kb() — device resumed
};

void boot_trace(uint32_t stage);

// Writes an arbitrary 32-bit value to a dedicated diagnostic region at
// 0x08021000 + slot*4 (write-once per boot; erased = 0xFFFFFFFF means the slot
// was never written). Slots are documented next to their call sites in
// he_matrix.c — they carry ADC samples, scan counts at USB milestones, and the
// first-detected keypress.
void boot_trace_value(uint32_t slot, uint32_t value);

// Records main-loop liveness. index is a monotonically increasing counter
// (capped); each distinct index < 512 is written once at 0x08020100 + index*4
// with value (0xBB000000 | index). After boot, the number of consecutive
// programmed words tells us how many heartbeat intervals the main loop
// survived (0/1 => crashed early, many => running continuously).
void boot_trace_heartbeat(uint32_t index);
