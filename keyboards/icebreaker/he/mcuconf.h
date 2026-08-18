/* Copyright 2024 Serene Industries
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Enable ADC1 (PA3 / ADC1_IN3) for the analog (Hall effect) matrix.
 */

#pragma once

#include_next <mcuconf.h>

#undef STM32_ADC_USE_ADC1
#define STM32_ADC_USE_ADC1 TRUE

#undef STM32_PWM_USE_TIM1
#define STM32_PWM_USE_TIM1 TRUE
