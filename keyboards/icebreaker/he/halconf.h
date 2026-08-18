/* Copyright 2024 Serene Industries
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Enable the ChibiOS ADC driver for the analog (Hall effect) matrix.
 */

#pragma once

#define HAL_USE_ADC TRUE
#define HAL_USE_PWM TRUE

#include_next <halconf.h>
