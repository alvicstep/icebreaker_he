/* Copyright 2024 Serene Industries (recovered pin map)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Flash-backed boot-stage tracing. See boot_trace.h for the rationale.
 *
 * We deliberately avoid ChibiOS HAL calls here: the earliest trace points run
 * from crt0 before halInit(), so we program the STM32F4 flash controller
 * directly. Each marker is a single 32-bit word; a stage is written at most
 * once per boot (erased = 0xFFFFFFFF), so the highest non-0xFF marker in the
 * trace region tells us exactly where the boot sequence stopped.
 */

#include "boot_trace.h"

#include "hal.h"

// Sector 5 on STM32F411 = 0x08020000..0x0803FFFF (128 KB). It lies entirely
// beyond the firmware image (which ends below 0x08020000), so erasing it never
// touches running code. The 128 KB erase is ~1 s and is only done once,
// defensively, if the region is not already blank.
#define BOOT_TRACE_BASE   0x08020000UL
#define BOOT_TRACE_SECTOR 5U

void boot_trace(uint32_t stage) {
    if (stage > 63U) {
        return;
    }

    volatile uint32_t *addr = (volatile uint32_t *)(BOOT_TRACE_BASE + (stage * 4U));

    // One marker per stage per boot — skip if already programmed.
    if (*addr != 0xFFFFFFFFUL) {
        return;
    }

    // Wait for any pending flash operation to finish, then unlock.
    while (FLASH->SR & FLASH_SR_BSY) {}
    FLASH->KEYR = 0x45670123UL;
    FLASH->KEYR = 0xCDEF89ABUL;

    // Defensive: erase the trace sector once if it isn't blank (e.g. if the
    // DFU download only did a sector erase and left old data here).
    if (*(volatile uint32_t *)BOOT_TRACE_BASE != 0xFFFFFFFFUL) {
        FLASH->CR &= ~FLASH_CR_SNB;
        FLASH->CR |= FLASH_CR_SER | (BOOT_TRACE_SECTOR << FLASH_CR_SNB_Pos);
        FLASH->CR |= FLASH_CR_STRT;
        while (FLASH->SR & FLASH_SR_BSY) {}
        FLASH->CR &= ~FLASH_CR_SER;
    }

    // Program one 32-bit word (PSIZE = 0b10 = word).
    FLASH->CR &= ~(FLASH_CR_PSIZE_0 | FLASH_CR_PSIZE_1);
    FLASH->CR |= FLASH_CR_PSIZE_1;
    FLASH->CR |= FLASH_CR_PG;
    *addr = 0xAA000000UL | stage;
    while (FLASH->SR & FLASH_SR_BSY) {}
    FLASH->CR &= ~FLASH_CR_PG;

    FLASH->CR |= FLASH_CR_LOCK;
}

/* Arbitrary 32-bit value region (ADC samples, scan counts at USB milestones,
 * first keypress, min/max raw, suspend/wakeup counts). Lives at 0x08021000,
 * safely inside sector 5 and clear of both the 64 boot markers (0x08020000)
 * and the heartbeat region (0x08020100..0x08020900). Write-once per boot. */
#define BT_VALUE_BASE 0x08021000UL
#define BT_VALUE_MAX  32U

void boot_trace_value(uint32_t slot, uint32_t value) {
    if (slot >= BT_VALUE_MAX) {
        return;
    }

    volatile uint32_t *addr = (volatile uint32_t *)(BT_VALUE_BASE + (slot * 4U));

    // Write-once semantics.
    if (*addr != 0xFFFFFFFFUL) {
        return;
    }

    // Wait for any pending flash operation to finish, then unlock.
    while (FLASH->SR & FLASH_SR_BSY) {}
    FLASH->KEYR = 0x45670123UL;
    FLASH->KEYR = 0xCDEF89ABUL;

    // Program one 32-bit word (PSIZE = 0b10 = word).
    FLASH->CR &= ~(FLASH_CR_PSIZE_0 | FLASH_CR_PSIZE_1);
    FLASH->CR |= FLASH_CR_PSIZE_1;
    FLASH->CR |= FLASH_CR_PG;
    *addr = value;
    while (FLASH->SR & FLASH_SR_BSY) {}
    FLASH->CR &= ~FLASH_CR_PG;

    FLASH->CR |= FLASH_CR_LOCK;
}

/* Main-loop heartbeat region sits just past the 64 boot-stage markers. Each
 * distinct index is programmed once (erased = 0xFFFFFFFF), so the number of
 * consecutive programmed words after boot equals the number of heartbeat
 * intervals the main loop completed. */
#define BT_HEARTBEAT_BASE 0x08020100UL
#define BT_HEARTBEAT_MAX  512U

void boot_trace_heartbeat(uint32_t index) {
    if (index >= BT_HEARTBEAT_MAX) {
        return;
    }

    volatile uint32_t *addr = (volatile uint32_t *)(BT_HEARTBEAT_BASE + (index * 4U));

    // Write-once semantics.
    if (*addr != 0xFFFFFFFFUL) {
        return;
    }

    // Wait for any pending flash operation to finish, then unlock.
    while (FLASH->SR & FLASH_SR_BSY) {}
    FLASH->KEYR = 0x45670123UL;
    FLASH->KEYR = 0xCDEF89ABUL;

    // Program one 32-bit word (PSIZE = 0b10 = word).
    FLASH->CR &= ~(FLASH_CR_PSIZE_0 | FLASH_CR_PSIZE_1);
    FLASH->CR |= FLASH_CR_PSIZE_1;
    FLASH->CR |= FLASH_CR_PG;
    *addr = 0xBB000000UL | index;
    while (FLASH->SR & FLASH_SR_BSY) {}
    FLASH->CR &= ~FLASH_CR_PG;

    FLASH->CR |= FLASH_CR_LOCK;
}

/* --------------------------------------------------------------------------
 * CPU fault handlers. ChibiOS declares these weak and points them at a bare
 * `while(1)`; we override them to leave a flash marker before halting so a
 * crash is distinguishable from a clean-but-silent boot.
 * ------------------------------------------------------------------------ */

void HardFault_Handler(void) {
    boot_trace(BT_FAULT_HARD);
    while (true) {}
}

void MemManage_Handler(void) {
    boot_trace(BT_FAULT_MEMMANAGE);
    while (true) {}
}

void BusFault_Handler(void) {
    boot_trace(BT_FAULT_BUS);
    while (true) {}
}

void UsageFault_Handler(void) {
    boot_trace(BT_FAULT_USAGE);
    while (true) {}
}

/* --------------------------------------------------------------------------
 * Early boot hooks (all weak in tmk_core/protocol/chibios/chibios.c; these
 * strong definitions override them so we can trace before USB exists).
 * ------------------------------------------------------------------------ */

void early_hardware_init_pre(void) {
    boot_trace(BT_EARLY_PRE_ENTRY);
#if EARLY_INIT_PERFORM_BOOTLOADER_JUMP
    void enter_bootloader_mode_if_requested(void);
    enter_bootloader_mode_if_requested();
#endif
    boot_trace(BT_EARLY_PRE_EXIT);
}

void early_hardware_init_post(void) {
    boot_trace(BT_EARLY_POST_ENTRY);
    boot_trace(BT_EARLY_POST_EXIT);
}

void board_init(void) {
    boot_trace(BT_BOARD_INIT_ENTRY);
    boot_trace(BT_BOARD_INIT_EXIT);
}
