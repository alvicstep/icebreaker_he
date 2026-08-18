# Build options for the Icebreaker HE reconstruction scaffold.
#
# Custom analog (Hall effect) matrix driver — see he_matrix.c.
CUSTOM_MATRIX = lite

# MCU / bootloader (set in keyboard.json too, kept here for the build system).
MCU = STM32F411
BOOTLOADER = stm32-dfu

# Sources
SRC += he_matrix.c
SRC += he_via.c

# Enable the QMK analog (ADC) driver — he_matrix.c uses analogReadPin().
ANALOG_DRIVER_REQUIRED = yes

# EEPROM persistence for per-key actuation/release thresholds (69 x 6 bytes)
# via the QMK keyboard data block. Backing size must be a whole flash sector.
EEPROM_DRIVER = wear_leveling
WEAR_LEVELING_DRIVER = embedded_flash

# Features (nkro/extrakey/mousekey/raw are set in keyboard.json; VIA is enabled
# per-keymap in keymaps/via/rules.mk).
# Encoder: A/B rotation = PB14/PB13 (config.h), push = matrix [4,9] (he_matrix.c).
# ENCODER_MAP_ENABLE lets VIA remap the CW/CCW actions per-layer.
ENCODER_ENABLE = yes
ENCODER_MAP_ENABLE = yes

# Hall-effect / rapid-trigger feature flags (custom, see he_matrix.c).
HE_ENABLE = yes

# Share the keyboard HID endpoint with extrakey/nkro/mousekey so those features
# fit within the STM32F411 OTG endpoint budget (alongside VIA raw HID + console).
KEYBOARD_SHARED_EP = yes

# Console (debug) output is declared via "console": true in keyboard.json.

# Do not enable Link Time Optimization until the driver is stable.
LTO_ENABLE = no
