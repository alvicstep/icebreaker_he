# Icebreaker HE — Official vs. Reverse-Engineered Firmware Comparison

> **Scope:** `firmware/boards/wired/icebreaker_hall_effect/` (official vendor source) vs. `qmk_firmware/keyboards/icebreaker/he/` (our reconstruction).
>
> **Date of analysis:** (generated during the comparison session)
>
> **Bottom line:** The official source is a **work-in-progress QMK port** (not the shipped ZMK binary we originally disassembled). It contains a large amount of dead code, TODO/FIXME comments, and several genuine bugs — most notably the exact one you hit: **actuation mode and rapid-trigger tuning are never restored from EEPROM, so they reset on every power-cycle/reflash.** Our reconstruction fixes that and several other issues, but it also drops a few of the official's niceties (3-state calibration LED, caps-lock LED, slider visualization, debug console) and diverges on ADC resolution.

---

## 1. Executive Summary

| # | Finding | Severity | Where |
|---|---------|----------|-------|
| 1 | **Actuation mode is never restored on boot** — `he_config.he_actuation_mode` is written to EEPROM but never read back into `he_config` in `he_init()`. It always boots as Normal (0). | 🔴 Critical | official `he_switch_matrix.c` `he_init()` |
| 2 | **Rapid-trigger tuning (deadzone/engage/disengage) is never persisted** — reset to `15/10/10` on every boot. `eeprom_he_key_rapid_trigger_config_t` is declared but never used. | 🔴 Critical | official `he_switch_matrix.c` `he_init()` |
| 3 | **"AD Mode" and "ZX Mode" are advertised in VIA but have no firmware handler** — VIA sends value IDs 10/11, firmware has no `case` for them; key-cancel is hardcoded A/D only. | 🟠 High | official `via_apc.c` + `icebreaker_he_via_definitions.json` |
| 4 | **Rapid-trigger re-press boundary math is wrong** — on release, `boundary = release + engage`, then re-press requires `> boundary + engage` (2× engage). | 🟠 High | official `he_update_key_rapid_trigger()` |
| 5 | **ADC runs at 10-bit, not 12-bit** — no `ADC_RESOLUTION` override, so QMK's analog driver defaults to 10-bit (0–1023). The shipped binary we disassembled used 12-bit. | 🟠 High | official `config.h` (omission) |
| 6 | **Normal-mode release is not debounced** — press is debounced (5 samples), release fires on the first sample. Asymmetric. | 🟡 Medium | official `he_update_key()` |
| 7 | **VIA `get_value` byte-packing is inconsistent** — only deadzone is written correctly; actuation/release/mode/engage/disengage write into the high byte. | 🟡 Medium | official `via_he_config_get_value()` |
| 8 | **Deadzone VIA `set_value` writes EEPROM 69× inside the loop** (redundant + wear), and saves the *wrong* struct. | 🟡 Medium | official `via_apc.c` ID 7 |
| 9 | **`id_custom_save` (VIA Save button) is a no-op** ("Bypass"). | 🟡 Medium | official `via_apc.c` |
| 10 | **`STM32_PWM_USE_ADVANCED TRUE` is vestigial** — not a recognized macro for F411XE or the TIMv1 LLD. | 🟢 Low | official `mcuconf.h` |
| 11 | Lots of dead code / dead globals (`cancel_lock`, `a/d_physically_pressed`, `rt_actuation_point`, `#ifdef matrix_shenanigans`, `via_update_config()` "is this ever called??"). | 🟢 Low | multiple |

### What our reconstruction does *better*
- ✅ Persists actuation mode **and** RT tuning across reboot (dedicated settings record + versioned KB datablock).
- ✅ Explicit 12-bit ADC (matches the shipped binary).
- ✅ Implements **both** A/D **and** Z/X key-cancel pairs (SOCD).
- ✅ Input clamping on every VIA value + clamp-on-load from EEPROM.
- ✅ `id_custom_save` actually commits staged thresholds (stock no-op fixed).
- ✅ `WS2812_T1H = 792` timing fix + `RGBLIGHT_LIMIT_VAL` brownout cap.
- ✅ `bootmagic_should_reset()` guard against spurious EEPROM/bootloader resets.
- ✅ Symmetric per-key debounce — 5-sample confirm on press **and** release (incl. encoder push).
- ✅ 2-second VIA auto-save fallback (`he_via_task()`).
- ✅ Boot auto-cal provisional span — keys register immediately on power-up.
- ✅ Trimmed per-key EEPROM record (6 → 2 bytes, `EECONFIG_KB_DATA_VERSION 3`).

---

## 2. Architecture Overview

Both are QMK firmware for the same STM32F411 board, but they are structured differently:

| Aspect | Official | Ours |
|--------|----------|------|
| Matrix driver | `CUSTOM_MATRIX = yes` (full custom `matrix.c` with `matrix_init()`/`matrix_scan()`) | `CUSTOM_MATRIX = lite` (QMK provides `matrix_init/scan`, we supply `matrix_init_custom()`/`matrix_scan_custom()`) |
| Encoder | Manually calls low-level `encoder_driver_init()`/`encoder_driver_task()` from `matrix.c` | Standard QMK `ENCODER_ENABLE` + `ENCODER_MAP_ENABLE` |
| EEPROM layout | KB datablock = 3 bytes (`{cal_mode, post_flash, mode}`), user datablock = 414 bytes (69 × 6-byte key records) | KB datablock = 142 bytes (69 × 2-byte key records + 4-byte settings), versioned (`EECONFIG_KB_DATA_VERSION 3`) |
| ADC | 10-bit (QMK default) | 12-bit (explicit `ADC_RESOLUTION`) |
| Key scan | Per-sensor dispatch by mode (0/1/2) in one function | Per-sensor state machine in `he_compute_pressed()` with SOCD resolution |
| RGB | `icebreaker_rgb.c` (3-state cal LED, slider viz, caps-lock LED, mode blink) | `he_matrix.c` integrated (2-state cal LED, per-key green latch) |

---

## 3. File-by-File Comparison

| Official file | Our equivalent | Verdict |
|---------------|----------------|---------|
| `hall_effect/config.h` | `config.h` | Mostly equivalent pins/timers; differs on ADC resolution, EEPROM sizes, RGB defaults, WS2812 timing |
| `hall_effect/he_switch_matrix.h` | `he_matrix.h` | Same data model (sensor map, key config, RT config); ours adds settings record + versioning |
| `hall_effect/he_switch_matrix.c` | `he_matrix.c` | Same sensor/mux/ADC table; different calibration, debounce, RT math, SOCD |
| `hall_effect/matrix.c` | `matrix.c` (QMK-lite) | Official is full-custom; ours delegates to QMK |
| `icebreaker_rgb.c/h` | *(folded into `he_matrix.c`)* | Official has richer RGB UX; ours is simpler |
| `hall_effect/icebreaker_he.c` | *(in `he_matrix.c`)* | Official `via_update_config()` is dead code |
| `hall_effect/keymaps/via/via_apc.c` | `he_via.c` | Same value IDs 1–9; ours fixes the bugs |
| `hall_effect/keymaps/via/keymap.c` | `keymaps/*/keymap.c` | Official has custom keycodes + blink + caps-lock LED |
| `keyboard.json` | `keyboard.json` | Same VID/PID/layout; official has labels + encoder + full features |
| `rules.mk` | `rules.mk` | Official uses `CUSTOM_MATRIX=yes`; ours `lite` + explicit EEPROM driver |
| `mcuconf.h` / `halconf.h` | same | Official adds vestigial `STM32_PWM_USE_ADVANCED` |

---

## 4. Detailed Implementation Differences

### 4.1 Sensor → matrix map — ✅ IDENTICAL
The 69-entry `sensor_to_matrix_map[]` (official) and `he_sensors[]` (ours) match entry-for-entry, including the dummy encoder entry `{4,9,68,0,0}` at index 68. Mux enable pins `{B0,A7,A6,A5,A4}`, select pins `{B3,B4,B6,B5}`, analog pin `PA3` all match. LED serpentine remap also matches.

### 4.2 ADC resolution — 🔴 DIVERGENCE
- **Official:** never defines `ADC_RESOLUTION`, so QMK's `analog.c` defaults to `ADC_CFGR1_RES_10BIT` (10-bit, raw 0–1023). Its raw constants (`EXPECTED_NOISE_FLOOR 540`, `EXPECTED_NOISE_CEILING 700`, calibration thresholds `570/600/635`) are all consistent with a 10-bit scale.
- **Ours:** explicitly sets `ADC_RESOLUTION ADC_CFGR1_RES_12BIT`, matching the **shipped binary** we disassembled (rest ≈ 2048/2150, bottom ≈ 2842).

> This means the official source reads Hall sensors at **4× lower analog resolution** than the firmware actually shipped on the board. It still works (the 10-bit travel swing is ~160 raw counts), but actuation/release thresholds are far coarser.

### 4.3 EEPROM layout
- **Official:** KB datablock `EECONFIG_KB_DATA_SIZE = 3` = `{he_calibration_mode, he_post_flash, he_actuation_mode}`. User datablock `EECONFIG_USER_DATA_SIZE = 414` = 69 × 6-byte `eeprom_he_key_config_t`. The RT config struct is declared (`eeprom_he_key_rapid_trigger_config_t`) but **never persisted or restored**.
- **Ours:** single 142-byte KB datablock (69 × 2-byte key records `{actuation, release}` + 4-byte `he_settings_eeprom_t {mode, deadzone, engage, release_dist}`), versioned with `EECONFIG_KB_DATA_VERSION 3` and guarded by `eeconfig_is_kb_datablock_valid()`. The recovered 6-byte record's never-read `reserved`/`engage`/`raw` bytes were dropped (the version bump forces a one-time reformat).

### 4.4 Calibration UX
- **Official:** 3-state per-key feedback — red (uncalibrated, ceiling < 570), orange (partial, ≥ 600), green (good, ≥ 635) — plus `calibration_warning()` flashing and a "fully press each key" VIA flow.
- **Ours:** 2-state (red until pressed past `HE_CAL_PRESS_MARGIN`, then green latch), plus boot auto-cal (64-scan rest sampling) and a `span < HE_ADC_MIN_SPAN` rejection fallback.

### 4.5 RGB / LED UX
- **Official** has several features we dropped: caps-lock indicator (LED index 31, red), mode-change blink, slider visualization (`start/update/end_slider_visualization`), and `RGBLIGHT_SLEEP`.
- **Ours** is intentionally minimal: teal static default (your preference, hue 114/sat 128/val 128), per-key green calibration latch.

---

## 5. Bugs in the OFFICIAL Firmware

### 🔴 5.1 Actuation mode never restored (`he_init()`)
`keymap.c` writes `eeprom_he_config.he_actuation_mode` and calls `eeconfig_update_kb_datablock()` (keycodes `APCM`/`RTM`/`KCM`), and the VIA dropdown (ID 6) writes `he_config.he_actuation_mode` (RAM only). But `he_init()` never copies `eeprom_he_config.he_actuation_mode` back into `he_config.he_actuation_mode`:

```c
// he_init() — else branch (post_flash == true):
he_key_configs[i].he_actuation_threshold = eeprom_he_key_configs[i].he_actuation_threshold;
he_key_configs[i].he_release_threshold   = eeprom_he_key_configs[i].he_release_threshold;
he_key_configs[i].noise_ceiling          = eeprom_he_key_configs[i].noise_ceiling;
// NOTE: he_config.he_actuation_mode is never restored here
```

**Result:** every boot starts in Normal mode regardless of what was saved. This is the root cause of "why do I have to set the mode every time."

### 🔴 5.2 Rapid-trigger tuning never persisted
`he_key_rapid_trigger_configs[i].deadzone/engage_distance/disengage_distance` are **always** reset to `DEFAULT_DEADZONE_RT (15)` / `DEFAULT_RELEASE_DISTANCE_RT (10)` in `he_init()`. The `eeprom_he_key_rapid_trigger_config_t` type is defined but never used. RT sliders in VIA change RAM only.

### 🟠 5.3 "AD Mode" / "ZX Mode" toggles exist in VIA but are unimplemented
`icebreaker_he_via_definitions.json` defines:
```json
{ "label": "AD Mode", "content": ["id_keycancel_ad_mode", 0, 10] },
{ "label": "ZX Mode", "content": ["id_keycancel_xz_mode", 0, 11] }
```
But `via_he_config_set_value()` has no `case` for IDs 10/11 (its enum ends at 9), so both buttons do nothing. `he_update_key_keycancel()` is hardcoded to A/D (`sensor_id == 32/34`) with no Z/X path.

### 🟠 5.4 Rapid-trigger re-press boundary doubled
In `he_update_key_rapid_trigger()`, on release:
```c
*boundary_value = rescaled_value + engage_distance;
```
Then re-press is evaluated as:
```c
bool currently_pressed = rescaled_value > (*boundary_value + engage_distance);
```
So a key must rise **2× `engage_distance`** above its release point to re-press (should be 1×). Combined with the release-time `boundary = release + engage`, the hysteresis behavior is inconsistent with the declared RT semantics.

### 🟡 5.5 Normal-mode release is not debounced
`he_update_key()` debounces press (`++debounce_counter >= DEBOUNCE_THRESHOLD`) but releases immediately on the first `should_release` sample. RT mode debounces both, normal mode debounces only press — inconsistent.

### 🟡 5.6 VIA `get_value` byte-packing bug
In `via_he_config_get_value()` (recall `data[0]=value_id`, `data[1]=high byte`, `data[2]=low byte` for the 16-bit VIA protocol):
- Deadzone (ID 7): `value_data[0] = deadzone >> 8; value_data[1] = deadzone & 0xFF;` ✅ correct (high=0, low=value).
- Actuation/release/mode (IDs 1/2/6): `*value_data = value;` writes into the **high** byte, low byte left undefined ❌.
- Engage/disengage (IDs 8/9): `value_data[0] = value & 0xFF;` writes into the **high** byte, low byte undefined ❌.

So only deadzone round-trips correctly; the others report `value << 8` (or garbage).

### 🟡 5.7 Deadzone `set_value` writes EEPROM 69× with the wrong struct
```c
case id_set_rapid_trigger_deadzone: {
    for (int i = 0; i < SENSOR_COUNT; i++) {
        he_key_rapid_trigger_configs[i].deadzone = value_data;
        eeconfig_update_user_datablock(&eeprom_he_key_configs);   // inside the loop!
    }
    ...
}
```
Writes the full 414-byte user datablock 69 times (redundant + flash wear), and it saves `eeprom_he_key_configs` (key thresholds) — not the RT configs it just changed.

### 🟡 5.8 `id_custom_save` is a no-op
```c
case id_custom_save: {
    // Bypass
    break;
}
```
The VIA "Save" button does nothing. Persistence relies on the 2-second `eeprom_save_pending` timer in `matrix_scan_user()` (and even then only for thresholds, never for mode/RT).

### 🟢 5.9 Vestigial `STM32_PWM_USE_ADVANCED`
Official `mcuconf.h` sets `STM32_PWM_USE_ADVANCED TRUE`. This macro is **not defined** in the F411XE mcuconf nor in the ChibiOS TIMv1 LLD, and PA8 is TIM1_CH1 (a standard channel, not advanced). Harmless but meaningless.

### 🟢 5.10 Dead code
`cancel_lock` (declared, never used), `a_physically_pressed`/`d_physically_pressed` (set, never read), `rt_actuation_point` field (never used), `via_update_config()` in `icebreaker_he.c` (commented "is this ever called??"), `#ifdef matrix_shenanigans` block in the default keymap, and numerous `// delete this?` / `// todo` comments.

---

## 6. Bugs / Improvement Opportunities in OUR Firmware

These are self-critiques — places where the official version (or a cleaner design) is arguably better.

1. **Missing 3-state calibration feedback.** Official shows red→orange→green (570/600/635 thresholds) and flashes low-ceiling sensors. Ours is binary red→green. Adopting the official's orange "partial" state + `calibration_warning()` flash would improve UX. *(If ported, thresholds must be scaled to 12-bit: ×4.)*

2. **No caps-lock LED indicator.** Official lights LED index 31 red when caps lock is on. We dropped it. If you relied on it, it's a regression.

3. **No slider visualization.** Official previews threshold changes on the LED strip while dragging VIA sliders. We dropped this.

4. **No debug console.** Official has extensive `console_output` levels + sensor statistics (mean/std-dev). Ours has none — fine for production, but it makes tuning harder.

> ✅ **Items 5–10 below were fixed** in the hardening pass (see §8). They are kept here for provenance.

5. ~~**Per-key EEPROM `engage`/`raw` fields are redundant.**~~ → **DONE:** per-key record is now 2 bytes (`{actuation, release}`); the never-read `reserved`/`engage`/`raw` bytes are dropped.

6. ~~**No explicit per-key debounce counter for analog keys.**~~ → **DONE:** symmetric `HE_DEBOUNCE 5` confirm on both press and release (`he_debounced[]`), including the encoder push.

7. ~~**Rapid-trigger is a clean-room reimplementation.**~~ → **DONE (validated):** documented equivalence to the official `boundary_value` model and the official's 2× engage re-press bug it avoids.

8. ~~**No 2-second auto-save fallback.**~~ → **DONE:** `he_via_task()` + `HE_VIA_AUTOSAVE_MS 2000` auto-persist staged slider changes.

9. ~~**Boot auto-cal window (first 64 scans).**~~ → **DONE:** provisional span seeds `raw_min = mid − margin`, `raw_max = raw_min + span`, so keys register immediately on power-up.

10. ~~**`keymaps/via/` is an incomplete stub.**~~ → **DONE:** removed; only `default/` and `via_custom/` remain.

---

## 7. What Our Firmware Does Better (Improvements over Stock)

| Improvement | Why it matters |
|-------------|----------------|
| Persists **mode + RT tuning** to EEPROM | Fixes the exact "mode resets every flash" bug (stock never restores mode; never persists RT) |
| Explicit **12-bit ADC** | Matches the shipped binary; 4× more travel resolution than stock's 10-bit |
| Implements **Z/X key-cancel** (SOCD pairs `{32,34}` and `{46,47}`) | Stock advertises ZX mode but never implements it |
| `id_custom_save` **commits** staged thresholds | Stock's Save button is a no-op |
| **Clamps every VIA value** + clamp-on-load | Stock trusts VIA input and EEPROM contents |
| `WS2812_T1H = 792` | Recovered vendor timing; prevents strip data corruption |
| `RGBLIGHT_LIMIT_VAL = 128` | Brownout prevention on full-brightness white |
| `bootmagic_should_reset()` override | VIA forces `BOOTMAGIC_ENABLE`, which can false-trigger on uncalibrated Hall rest readings |
| `keyboard_pre_init_kb()` formats EEPROM before USB | Avoids `GET_DESCRIPTOR` timeout on first boot |
| `eeconfig_is_kb_datablock_valid()` + versioning | Safe, explicit EEPROM schema management |
| Symmetric per-key debounce (`HE_DEBOUNCE 5`) | Filters sensor/ADC noise on both edges; official only debounced press |
| 2-second VIA auto-save fallback | Persists staged sliders even if VIA never sends the save command |
| Boot auto-cal provisional span | Keys register immediately on power-up (before the 64-scan lock-in) |
| Trimmed EEPROM record (6 → 2 bytes) | Drops never-read recovered fields; `EECONFIG_KB_DATA_VERSION 3` |

---

## 8. Feature Completeness — Implemented vs. Missing

### 8.1 Implemented in our firmware ✅

| Area | Detail |
|------|--------|
| Matrix | 5×16 analog Hall matrix via 5× 74HC4067 muxes, 69 sensors (68 Hall + encoder push), 12-bit ADC |
| Sensor map | 69-entry table verified byte-for-byte identical to the official (`he_sensors[]`) |
| Encoder | Rotary (PB14/PB13) + push GPIO (PB12/PB15) via standard QMK `ENCODER_ENABLE` |
| Actuation modes | Normal, Rapid Trigger, Key Cancel (SOCD) |
| Key cancel | **Both** A/D (`{32,34}`) and Z/X (`{46,47}`) pairs — official only implements A/D |
| RT semantics | Peak/valley model equivalent to official `boundary_value`, minus its 2× engage bug |
| EEPROM | Per-key thresholds (2 B × 69) + mode + RT tuning, versioned datablock (`v3`) |
| VIA IDs | 1 (actuation), 2 (release), 3 (save), 4/5 (cal), 6 (mode), 7 (deadzone), 8 (engage), 9 (release-dist), 12 (set-all) |
| VIA save | `id_custom_save` commits staged thresholds + persists mode/tuning |
| Auto-save | 2 s fallback (`he_via_task()`) if VIA never sends the save command |
| Debounce | Symmetric 5-sample confirm on press **and** release (incl. encoder push) |
| Calibration | Manual red→green per-key latch + 64-scan boot auto-cal with provisional span |
| RGB | Teal static default, `WS2812_T1H=792`, `RGBLIGHT_LIMIT_VAL=128` brownout cap |
| Robustness | `bootmagic_should_reset()` guard, `keyboard_pre_init_kb()` EEPROM format before USB |

### 8.2 Missing vs. the official ❌

| Feature | Official behaviour | Status / note |
|---------|--------------------|---------------|
| 3-state calibration LED | red→orange→green (570/600/635) + `calibration_warning()` flash | Dropped (we use 2-state); port ×4 for 12-bit |
| Caps-lock LED | LED index 31 lit red | Dropped |
| Slider visualization | LED strip previews threshold drags | Dropped |
| Debug console | `console_output` levels + sensor mean/std-dev stats | Dropped |
| Mode-change blink | Blinks the strip on mode switch | Dropped |
| `RGBLIGHT_SLEEP` | Suspends RGB on sleep | Dropped |
| Separate A/D / Z/X VIA toggles | IDs 10/11 (`id_keycancel_ad_mode`, `id_keycancel_xz_mode`) | N/A — our Key Cancel covers both pairs; official toggles were dead code |

### 8.3 Deliberately *not* ported

- `CUSTOM_MATRIX=yes` + manual low-level `encoder_driver_init()` calls — QMK `lite` matrix + `ENCODER_ENABLE` is cleaner.
- 10-bit ADC default — we match the shipped 12-bit binary.
- Buggy RT boundary math (2× engage re-press dead-band).
- `id_custom_save` no-op and deadzone 69× EEPROM write-in-loop.
- Unused `eeprom_he_key_rapid_trigger_config_t` and other dead globals.

---

## 9. Recommendations

1. **Keep our EEPROM persistence model** — it's the single most important fix over stock.
2. **Port the official's 3-state calibration LED + `calibration_warning()`** (scaled ×4 to 12-bit) for a better calibration UX.
3. **Optionally restore the caps-lock LED** and **slider visualization** if you miss them.
4. ~~Clean up the dead per-key EEPROM fields (`engage`, `raw`) and the stale `keymaps/via` stub.~~ ✅ **DONE** — record is now 2 bytes; `keymaps/via/` removed.
5. ~~Validate our RT `peak`/`valley` math against the official's *intended* semantics.~~ ✅ **DONE** — equivalence documented, official's 2× engage bug avoided.
6. ~~Consider a symmetric debounce.~~ ✅ **DONE** — symmetric `HE_DEBOUNCE 5` on press and release.
7. **Do not adopt** the official's `CUSTOM_MATRIX=yes` + manual low-level encoder driver calls, its 10-bit ADC default, or its buggy RT boundary math.

---

## 10. Appendix — Verified Identical Items

These were confirmed byte-for-byte / value-for-value equivalent between the two trees:
- Sensor→matrix map (69 entries) and LED serpentine remap.
- Mux CE pins `{B0, A7, A6, A5, A4}`, select pins `{B3, B4, B6, B5}`, analog pin `PA3`.
- Encoder rotary pins `B14/B13`, push pins `B12/B15`.
- USB VID/PID `0x5363:0x0010`, `device_version 0.0.1`.
- WS2812 data pin `PA8` (TIM1_CH1), PWM+DMA channel selectors, 68 LEDs, GRB order.
- VIA raw-HID channel 0, protocol v12, custom value IDs 1–9.
