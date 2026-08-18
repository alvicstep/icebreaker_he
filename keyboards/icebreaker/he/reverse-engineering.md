# Icebreaker HE — reverse-engineering provenance

How every constant and algorithm in this folder was recovered, so a future
session can re-derive or cross-check a fact against the original binary. The
vendor source is **unpublished** — everything below came from a DFU flash dump
and disassembly.

---

## Dumping & disassembling the original firmware

```sh
# board in DFU mode
dfu-util -a 0 -s 0x08000000:524288 -U icebreaker_he_flash.bin   # full 512 KB

# disassemble (ARM Thumb, no ELF headers — raw binary)
arm-none-eabi-objdump -D -b binary -m armv7e-m -M force-thumb \
    --adjust-vma=0x08000000 icebreaker_he_flash.bin > icebreaker_he.dis
```

- **Original image SHA256**: `5000be8cd361276b5a34122891052c6138ca0323995b23ed3eaeefd5f9035e86`
  (524288 bytes). A permanent backup lives at
  `~/icebreaker-firmware-backup/icebreaker_he_original.bin` (plus option bytes
  + README). Restore: `dfu-util -a 0 -s 0x08000000:leave -D ~/icebreaker-firmware-backup/icebreaker_he_original.bin`.

---

## Recovered facts → where they came from

| Fact | Source address / register | Evidence |
|---|---|---|
| **MCU = STM32F411** (not F401) | `PLLCFGR = 0x04401804` | PLLM=4, PLLN=96, PLLP=/2, src=HSE(8 MHz) → **96 MHz SYSCLK** (F401 max is 84 MHz ⇒ F411). HCLK=96, APB1=48, APB2=96. `FLASH_ACR=0x103` (3 WS + prefetch/cache). |
| 512 KB flash / 128 KB SRAM | — | DFU geometry (4×16K + 1×64K + 3×128K). |
| **ADC input = PA3** (ADC1_IN3) | `0x0800f9b0` (line cfg) → stored to `0x200011B6`; consumed by `adcConvert` wrapper `0x0800fa70` | Two halfwords written in `matrix_init`. |
| **Mux CE lines** = `[PB0, PA7, PA6, PA5, PA4]` | table @ `0x08013BD8` (5×u32) | one-hot: all HIGH, selected mux's CE LOW. |
| **Mux address lines S0–S3** = `[PB3, PB4, PB6, PB5]` | table @ `0x08013BEC` (4×u32) | S0=PB3, S1=PB4, S2=PB6, S3=PB5. |
| **Sensor table** (69 × 5 B) | @ `0x08013BFC` | `[row, col, sensor_id, mux_enable_idx(0-4), mux_addr(0-15)]`. |
| **`read_sensor`** | @ `0x08009850` | select CE + drive addr, then ADC convert on PA3. |
| **Noise-floor calibration** | @ `0x080098E0` | 10 samples/sensor, track min, store 6-byte structs. |
| **`matrix_init`** | @ `0x080096F8` | configures addr + CE lines; sets defaults (actuation 50, release 30, `byte4=700`=`0x2BC`, `byte3=10`). |
| **Encoder A/B** = PB14/PB13 | init fn @ `0x0800E484` | selector 0→PB14, 1→PB13, mode 0x20 (input pull-up). |
| **Quadrature LUT** | @ `0x08014F20` | `{0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0}` — standard QMK. |
| **Encoder push** = PB12 (drive) + PB15 (sense) | read @ `0x08009E18`; debounce @ `0x08009E3C` | drive PB12 HIGH, read PB15 (pull-down; pressed=HIGH). 5-sample confirm. |
| **Encoder special-case in scan** | `matrix_scan` @ `0x08009EDC` | `sensor_id == 68` → GPIO push read, not analog. |
| **encoder_update** | `0x0800E394` → `0x0800E332` | standard QMK encoder action queue; rotations only on layer 0. |

---

## Encoder push / mux collision

Sensor 68 (`matrix [4,9]`, the encoder push) decodes to **mux 0, address 0** —
the same channel as sensor 18 (`matrix [1,2]`). The push is **not** read
through the mux (its `(0,0)` bytes are dummy); `he_matrix.c` special-cases
index 68 to read the PB12/PB15 GPIO pair, leaving the analog `(0,0)` channel
free for key `[1,2]`. 68 of 69 `(mux, addr)` pairs are unique.

---

## EEPROM layout (recovered)

69 × 6 bytes = 414 B of per-key records, plus a 4-byte settings record at the
tail, in QMK's keyboard data block (`EECONFIG_KB_DATA_SIZE = HE_SENSOR_COUNT * 6 + 4`):

```c
typedef struct __attribute__((packed)) {
    uint8_t  actuation;   // 50
    uint8_t  release;     // 30
    uint8_t  reserved;    // 0
    uint8_t  engage;      // 10
    uint16_t raw;         // 0x02BC (700) — recovered default
} he_eeprom_key_config_t;
```

Only `actuation`/`release` are ever updated (VIA "Save" ID 3). The other four
bytes keep their recovered defaults.

> **Divergence (bug fix):** the original firmware never stored the actuation
> mode or the rapid-trigger tuning (deadzone / engage / release-distance) in
> EEPROM, so they reset on every power-cycle. This reconstruction appends a
> 4-byte `he_settings_eeprom_t` record (mode, deadzone, engage, release_dist)
> after the 69 per-key records and persists it on change.

---

## RGB lighting (recovered)

The board has a **68-LED WS2812/SK6812** addressable strip. The original
firmware drives it with **PWM + DMA** (not bit-banged), using QMK's `rgblight`
(not `rgb_matrix`):

| Fact | Source address | Evidence |
|---|---|---|
| Data pin = **PA8** (TIM1_CH1, AF1) | `ws2812_init` @ `0x0800E5D0` | PA8 set to alternate push-pull, very-high-speed. |
| Timer = **TIM1**, channel 1 (main output) | `ws2812_init` @ `0x0800E5D0` | CCR[0] written; advanced timer, no complementary pin. |
| DMA = **DMA2 Stream 5** (base `0x40026488`) | `ws2812_init` @ `0x0800E5D0` | stream CR/NDTR/PAR configured for TIM1_UP. |
| LED count = **68** | buffer @ `ws2812_init` | 1632 halfwords = 68 × 24 bits. |
| Frame = 1856 halfwords | `NDTR = 1856` | 1632 colour + 224 reset bits (224 × 1.25 µs = 280 µs = `WS2812_TRST_US`). |
| Bit encoding = 24 halfwords/LED | encode @ `0x0800E660` | "1" = `0x26`, "0" = `0x10` duty-cycle values. |
| Colour order = **GRB** | `ws2812_setleds` @ `0x0800E6B0` | green byte first. |
| Driver table | @ `0x08014E18` | function-pointer table (init/setleds) → `rgblight`. |

QMK maps this 1:1 onto `WS2812_DRIVER = pwm` (`ws2812_pwm.c`) in `config.h`:

```c
#define WS2812_PWM_DRIVER        PWMD1
#define WS2812_PWM_CHANNEL       1
#define WS2812_PWM_PAL_MODE      1      // PA8 AF1
#define WS2812_PWM_DMA_STREAM    STM32_DMA2_STREAM5
#define WS2812_PWM_DMA_CHANNEL   6      // TIM1_UP
```

plus `"rgblight": {"led_count": 68}` and `"ws2812": {"pin": "A8", "driver": "pwm"}`
in `keyboard.json`.

---

## Keymap & custom keycodes (recovered)

The full 3-layer keymap is a **contiguous `uint16_t` array @ flash `0x080146A6`**
(3 layers × 80 halfwords = 5 rows × 16 columns, matching `LAYOUT` in
`keyboard.json`). The original firmware has **three** layers, not four — the
"layer 3" seen in earlier naive decodes was actually the string pool.

**Keycode decode.** `0x7820`–`0x7828` are **standard QMK RGBlight keycodes**
(`QK_UNDERGLOW_*`), *not* vendor keycodes:

| Hex | QMK name | Alias | Hex | QMK name | Alias |
|---|---|---|---|---|---|
| `0x7820` | `QK_UNDERGLOW_TOGGLE` | `RGB_TOG` | `0x7825` | `QK_UNDERGLOW_SATURATION_UP` | `RGB_SAI` |
| `0x7821` | `QK_UNDERGLOW_MODE_NEXT` | `RGB_MOD` | `0x7826` | `QK_UNDERGLOW_SATURATION_DOWN` | `RGB_SAD` |
| `0x7822` | `QK_UNDERGLOW_MODE_PREVIOUS` | `RGB_RMOD` | `0x7827` | `QK_UNDERGLOW_VALUE_UP` | `RGB_VAI` |
| `0x7823` | `QK_UNDERGLOW_HUE_UP` | `RGB_HUI` | `0x7828` | `QK_UNDERGLOW_VALUE_DOWN` | `RGB_VAD` |
| `0x7824` | `QK_UNDERGLOW_HUE_DOWN` | `RGB_HUD` | | | |

The **real** vendor keycodes are `QK_KB_0`–`QK_KB_8` = `0x7E00`–`0x7E08`
(QMK's standard keyboard-keycode range, so no custom enum is needed):

| Keycode | Handler @ | Effect |
|---|---|---|
| `QK_KB_0` `0x7E00` | `0x0800ABFC` | **APC mode** — `he_set_mode(NORMAL)`, actuation 85, prints `Actuation Point Control Mode set` + `[PCB_SETTINGS]: APC MODE` |
| `QK_KB_1` `0x7E01` | `0x0800AC26` | **Rapid Trigger mode** — `he_set_mode(RAPID_TRIGGER)`, actuation 0, prints `Rapid Trigger Mode set` + `[PCB_SETTINGS]: RT MODE` |
| `QK_KB_2` `0x7E02` | `0x0800AC4C` | **Key Cancel (SOCD) mode** — `he_set_mode(KEY_CANCEL)`, actuation 170, prints `Key Cancel Mode set` |
| `QK_KB_3` `0x7E03` | `0x0800AC66` | Logging level 0 |
| `QK_KB_4` `0x7E04` | `0x0800AC7A` | Logging level 1 |
| `QK_KB_5` `0x7E05` | `0x0800AC8A` | Logging level 2 (blocking keystrokes) |
| `QK_KB_6` `0x7E06` | `0x0800AC9A` | Logging level 3 (none) |
| `QK_KB_7` `0x7E07` | `0x0800ACAA` | Logging level 4 |
| `QK_KB_8` `0x7E08` | `0x0800ACBA` | Logging level 5 |

**Handler** `process_record` @ `0x0800ABE4`: `sub.w r0, r0, #0x7E00;
cmp r0, #8; bhi -> return true`, then `tbb [pc, r0]` (table @ `0x0800ABF2`,
bytes `05 1A 2D 3A 44 4C 54 5C 64`). It acts on **key press only** and returns
`false` (consumed) for every `QK_KB_*` keycode.

**Decoded layers** (the two non-base layers):

- **Layer 1** (`MO(1)`, fn): `ESC F1..F12 TRNS TRNS RGB_TOG` /
  `TRNS×14 RGB_VAI` / `TRNS×12 MPLY RGB_VAD` /
  `MO(2) TRNS×6 RGB_HUD RGB_HUI RGB_SAD RGB_SAI MUTE VOLU TRNS` /
  `TRNS×6 MPRV VOLD MNXT TRNS`.
- **Layer 2** (`MO(2)`, settings): `QK_BOOT TRNS×15` /
  `TRNS QK_KB_0 QK_KB_1 QK_KB_2 TRNS×11` / `TRNS×14` /
  `TRNS QK_KB_4 QK_KB_8 QK_KB_3 QK_KB_5 TRNS×9` / `TRNS×10`.

The settings layer therefore maps **Q/W/E** → APC / Rapid Trigger / Key Cancel
and **Z/X/C/V** → logging 1 / 5 / 0 / 2. `QK_KB_6`/`QK_KB_7` (logging 3/4) are
keycode-only and not placed on the keymap.

**VIA custom values** — the original exposes exactly IDs **1–9** plus **12**
(`set_all`); IDs 10–11 and 13–16 are unhandled (SET falls through silently).
`get_value` @ `0x0800A800`, `set_value` @ `0x0800A8F0` (both `tbb`-dispatched).

---

## What was *not* recovered

- SOCD A&D / Z&X as *independent* VIA IDs — they exist only as the mode-2
  "Key Cancel" behaviour (ID 6). IDs 10–16 are silent on SET → not exposed.
- The vendor's VIA definitions asset (`assets/icebreaker_HE_via_definitions.json`
  on the configurator site) — 404; the JSON in `../` was reconstructed by probing.
