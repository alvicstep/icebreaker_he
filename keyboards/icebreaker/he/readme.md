# Icebreaker HE (reconstruction)

> ⚠️ **This is a reverse-engineered firmware, NOT the original vendor source.**
>
> Serene Industries' Icebreaker HE ships an **unpublished QMK fork**. The
> contents of this folder were recovered by dumping the 512 KB STM32 flash over
> DFU and disassembling it. The recovered advanced features (rapid trigger,
> actuation-point control, SOCD, calibration, EEPROM persistence) are
> implemented here and have been **validated on live hardware** (the board
> enumerates as "Icebreaker HE" and registers keys). Keep the original vendor
> `.bin` backed up before flashing anything over a working board.
>
> For the bring-up log and the bugs that were hit along the way, see
> [`troubleshooting.md`](troubleshooting.md).

## Hardware architecture (recovered)

| Item | Value |
|------|-------|
| MCU | STM32F411 @ 96 MHz (HSE 8 MHz → PLLM=4, PLLN=96, PLLP=/2) |
| Flash / SRAM | 512 KB / 128 KB |
| Bootloader | STM32 DFU (`[0483:df11]`) |
| USB VID / PID | `0x5363` / `0x0010` |
| VIA protocol | v12, raw HID usage page `0xFF60` (interface 1) |
| Matrix | 5 × 16 analog (Hall effect), custom driver |
| Key count | 69 (68 Hall sensors + 1 rotary-encoder push) |
| Encoder | 1 × EC11: A=`PB14`, B=`PB13`, push=`PB12`/`PB15` → matrix `[4,9]` |

### Analog matrix (Hall effect)

69 Hall sensors are read through **5 × 74HC4067 16:1 analog multiplexers**, wired-OR
into a **single ADC input**. One 12-bit ADC conversion per sensor, multiplexed in
software.

- **ADC input pin = `PA3`** (ADC1_IN3).
- **Mux enable lines (`CE`, active-LOW)** — 5 pins, one per mux:
  `[PB0, PA7, PA6, PA5, PA4]` (index 0..4).
- **Mux address lines (`S0`–`S3`)** — 4 pins, driven as a 4-bit channel select:
  `S0=PB3, S1=PB4, S2=PB6, S3=PB5`.
- **Sensor table**: 69 entries × 5 bytes = `[row, col, sensor_id, mux_enable_idx, mux_addr]`.
  See `he_matrix.c`.

`read_sensor(i)` algorithm (recovered from `matrix_scan` @ flash `0x08009850`):

1. Raise all 5 `CE` lines (disable every mux).
2. Pull the selected mux's `CE` low (enable it).
3. Drive the 4-bit `S0`–`S3` value for the target channel.
4. Run one ADC conversion on `PA3` and return the 12-bit raw value.

### Rotary encoder (recovered from disassembly)

The board has a single EC11 rotary encoder (push + CW/CCW rotation):

| Pin | Role | Mode |
|-----|------|------|
| `PB14` | Encoder **A** (rotation) | input, pull-up |
| `PB13` | Encoder **B** (rotation) | input, pull-up |
| `PB12` | Push-button **drive** line | output, driven HIGH on read |
| `PB15` | Push-button **sense** line | input, pull-down; pressed = HIGH |

- **Push button** = `matrix [4,9]` (sensor index 68), read by driving `PB12`
  HIGH and sampling `PB15`. It is **not** on the analog mux — its sensor-table
  entry (`04 09 44 00 00`) carries dummy mux/address bytes.
- **Rotation** uses the standard QMK quadrature decoder (`ENCODER_A_PINS`/`B_PINS`
  in `config.h`), 1 encoder, `ENCODER_RESOLUTION 4`.
- CW/CCW map to volume up/down on layer 0 (via `encoder_map` in the keymaps;
  VIA can remap them). The original firmware exposes the encoder at VIA binary
  keymap position 46 / virtual 68 with rotations only on layer 0.

### RGB lighting (recovered)

The board has a **68-LED WS2812/SK6812** addressable strip (one LED per Hall
key; the encoder push has none), recovered from the original firmware:

| Item | Value |
|------|-------|
| LED count | 68 (GRB colour order) |
| Data pin | `PA8` = TIM1_CH1, alternate function 1, push-pull |
| Transport | PWM + DMA (`WS2812_DRIVER = pwm`) |
| DMA | DMA2 Stream 5 / Channel 6 (TIM1_UP) |
| Feature | `rgblight` (the vendor used rgblight, not rgb_matrix) |

The pin/timer/DMA selectors live in `config.h`; the LED count and feature are
declared in `keyboard.json` (`"rgblight": {"led_count": 68}` +
`"ws2812": {"pin": "A8", "driver": "pwm"}`).

### ⚠️ Known anomaly — encoder push shares a mux channel

Sensor index 68 (`matrix [4,9]`, the rotary-encoder push button) decodes to
**mux 0, address 0** — the exact same channel as index 18 (`matrix [1,2]`).
68 of the 69 `(mux, addr)` pairs are unique; only `(0, 0)` collides.

This is because the encoder push is **not** read through the analog mux (see the
encoder table above) — the `(0, 0)` bytes in its table entry are dummy data, and
`he_matrix.c` special-cases sensor 68 to read the `PB12`/`PB15` GPIO pair
instead. The analog `(0, 0)` channel is therefore free for key `[1,2]`.

### EEPROM layout

69 × `he_eeprom_key_config_t` (6 bytes each = 414 B) followed by one
`he_settings_eeprom_t` (4 bytes) = 418 B total. Defaults (from `matrix_init` @
`0x080096F8`): actuation `50`, release `30`, `byte4=700` (0x2BC), `byte3=10`;
mode `0`, deadzone `15`, engage `10`, release-distance `10`.

Thresholds persist via VIA "Save Thresholds" (value ID 3) or the generic VIA
save command (`id_custom_save` / `0x09`). The actuation **mode** is persisted
immediately on change; the rapid-trigger **tuning** (deadzone / engage /
release-distance) persists on the save action. In the **original** firmware
these were all **RAM-only** and reset to Normal / defaults on power-cycle or USB
disconnect — this reconstruction fixes that bug.

### VIA custom value IDs

| ID | Meaning | Range |
|----|---------|-------|
| 1 | Actuation threshold | 10–90 |
| 2 | Release threshold | 10–90 |
| 3 | Save thresholds (button) | — |
| 4 | Start calibration (button) | — |
| 5 | End + save calibration (button) | — |
| 6 | Actuation mode | 0=Normal, 1=Rapid Trigger, 2=Key Cancel |
| 7 | Deadzone | 15–60 |
| 8 | Engage distance | 5–20 |
| 9 | Release distance | 5–20 |
| 12 | Set all (batch) | — |

Custom keycodes (layer 3): `APCM`, `RTM`, `KCM`, `DEBUG0`–`DEBUG5`. The
"Firmware Update" key is `QK_BOOTLOADER` (`0x7C00`) on layer 2.

## Folder layout

```
he/
├── readme.md             this file
├── troubleshooting.md    bring-up log: enumeration bug, boot loop, calibration polarity
├── via.md                VIA custom-value protocol, probing, definitions JSON
├── reverse-engineering.md  provenance: disassembly addresses of every recovered fact
├── keyboard.json         metadata + 5×16 LAYOUT (69 keys + encoder)
├── config.h            pin-map constants + auto-calibration tunables
├── rules.mk            custom matrix + features
├── he_matrix.h/.c      analog-matrix driver (sensor table + scan state machine)
├── he_via.h/.c         VIA custom-value handler
└── keymaps/
    ├── default/        bare-bones keymap
    └── via/            VIA-enabled keymap
```

## Status / TODO

- [x] Recovered: MCU, clock, ADC pin, mux pin map, 69-entry sensor table, EEPROM layout, VIA IDs.
- [x] Encoder A/B rotation pins (PB14/PB13) and encoder-push GPIO (PB12/PB15).
- [x] Rapid trigger / key-cancel (SOCD) / actuation-point control logic.
- [x] Noise-floor calibration (recovered algorithm).
- [x] EEPROM persistence of actuation/release thresholds (69 × 6-byte data block).
- [x] 12-bit ADC configuration (`ADC_RESOLUTION ADC_CFGR1_RES_12BIT`).
- [x] USB enumeration (OTGv1 step-1 connect fix — see troubleshooting.md).
- [x] Hall press polarity + auto-calibration (keys register — see troubleshooting.md).
- [x] RGB lighting: 68-LED WS2812/SK6812 strip on `PA8` (TIM1_CH1, PWM + DMA), rgblight.

## Useful commands

```sh
# dump the reference flash (board must be in DFU mode)
dfu-util -a 0 -s 0x08000000:524288 -U icebreaker_he_flash.bin

# flash a built .bin back, then leave DFU
# (note: :leave does NOT reset into the app — physically replug afterward)
dfu-util -a 0 -s 0x08000000:mass-erase:force -D <firmware>.bin
dfu-util -a 0 -s 0x08000000:leave

# exit DFU mode without flashing: unplug / replug USB
```

### Hall-effect calibration (tunables)

The Hall sensors are bipolar and read **ADC up on press** with a ~700-count
swing (see `troubleshooting.md`). Boot-time auto-calibration samples each
sensor's resting floor and maps `rest → ~0%`, `full press → ~100%`. Tune via:

- `HE_ADC_TRAVEL_SPAN` (700) — the full-press swing in raw counts.
- `HE_ADC_REST_MARGIN` (20) — raw counts below the floor for the 0% reference.
- `HE_ADC_REST_SAMPLE_SCANS` (64) — how long the rest floor is sampled.
