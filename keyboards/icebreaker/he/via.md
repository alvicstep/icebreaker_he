# Icebreaker HE — VIA reference

How the recovered firmware exposes its Hall-effect settings over VIA, and how
to drive / probe them. This is the authoritative record of the custom-value
protocol; `he_via.c` implements it, `readme.md` only summarizes the ID table.

---

## Transport

- **VIA protocol version 12** (`0x000C`), raw-HID usage page `0xFF60` (interface 1).
- The board also exposes a **QMK console** (usage `0xFF31` / `0x74`). For
  reverse-engineering and debugging, the console `uprintf` output is the source
  of truth — **trust it over raw VIA echo buffers** (see "gotchas" below).
- Keycodes in VIA dynamic-keymap responses are **big-endian**:
  `keycode = (data[4] << 8) | data[5]` (e.g. Esc `0x0029` → bytes `00 29`).
- GET_KEYCODE / SET_KEYCODE (cmd `0x04` / `0x05`) payload is `[layer, row, col]`
  — **not** a flat offset. The real matrix is **5 × 16**.

### Raw commands seen

| Cmd | Meaning |
|---|---|
| `0x01` | GET_PROTOCOL_VERSION |
| `0x02` | GET_KEYBOARD_VALUE |
| `0x04` | DYNAMIC_KEYMAP_GET_KEYCODE |
| `0x05` | DYNAMIC_KEYMAP_SET_KEYCODE |
| `0x18` | GET_LAYER_COUNT (returns `0xFF` = unhandled — older/custom subset) |

### Custom value commands (channel 0)

`via_custom_value_command_kb()` receives `data = [command_id, channel_id, value_id_and_data…]`.
The custom-value sub-commands are QMK's `id_custom_set_value = 0x07`,
`id_custom_get_value = 0x08`, `id_custom_save = 0x09`, on `id_custom_channel = 0`.
`value_id_and_data = [value_id, value_data…]`.

---

## Custom value IDs

| ID | Setting | Type | Default | Notes |
|---|---|---|---|---|
| 1 | Actuation threshold | value (`data[0]`) | 50 | 10–90. **Staged** — SET only updates RAM; commits on ID 3. |
| 2 | Release threshold | value (`data[0]`) | 30 | 10–90. Staged like ID 1. |
| 3 | **Save thresholds** | button | — | Commits ID 1/2 → all 69 sensors + EEPROM. Prints `Actuation Settings Saved!`. |
| 4 | **Start calibration** | button | — | "fully press each key… end in VIA". |
| 5 | **End calibration** | button (SET-only) | — | GET = "Unhandled ID 5" (no get case); SET-only. |
| 6 | Actuation mode | value (`data[0]`) | 0 | **0=Normal, 1=Rapid Trigger, 2=Key Cancel (SOCD)**. Direct set. **Persisted immediately.** |
| 7 | Deadzone | value | 15 | 15–60 (rapid trigger). **Persisted on save (ID 3 / 0x09).** |
| 8 | Engage distance | value (`data[0]`) | 10 | 5–20 (rapid trigger). **Persisted on save (ID 3 / 0x09).** |
| 9 | Release distance | value (`data[0]`) | 10 | 5–20 (rapid trigger). **Persisted on save (ID 3 / 0x09).** |
| 12 | Set all sensors | value (`data[0]`) | — | SET-only. Applies actuation to all 69 sensors with release = actuation−1, auto-saves. |

### Byte layout

- GET: every value (IDs 1, 2, 6, 7, 8, 9) is written to `value_id_and_data[1]`
  (the byte after the value id) as a single 8-bit byte.
- SET: reads `value_id_and_data[1]` (the byte after the value id) for all IDs.
- Custom keycodes (layer 3): `APCM`, `RTM`, `KCM`, `DEBUG0`–`DEBUG5`;
  non-standard `0x7820`–`0x7828` on layer 3 bottom row (offsets 32–40, not
  standard `QK_USER` `0x7E00`). "Firmware Update" = `QK_BOOTLOADER` (`0x7C00`).

---

## Faithful (reproduced) behaviours

These quirks are intentional — they mirror the original firmware:

- **Thresholds are staged.** SET (IDs 1/2) only stores a staged value; the
  "Save" button (ID 3) commits them to every sensor and writes EEPROM. The
  staged values are only applied when they were actually SET (dirty flag), so a
  save that follows a mode/tuning-only change does not reset the per-key
  thresholds to the staged defaults.
- **"Set all" (ID 12)** applies immediately and auto-saves, with
  `release = actuation − 1` (recovered behaviour).

## Divergences from the original (bug fixes)

The original firmware had several persistence bugs, fixed here:

- **Actuation mode and rapid-trigger tuning are persisted.** The original kept
  them RAM-only, so they reset to Normal / defaults on power-cycle or USB
  disconnect. We now write them to a 4-byte settings record at the tail of the
  keyboard EEPROM block (after the 69 per-key records). The mode is persisted
  immediately on change (discrete toggle); the tuning values (slider values) are
  persisted on the save action (ID 3 or `0x09`) to avoid hammering the
  flash-backed EEPROM during a VIA drag.
- **The generic save command (`id_custom_save` / `0x09`) is implemented.** The
  original had no handler, so the standard VIA "Save" button did nothing; it
  now persists the current state (per-key thresholds + mode + tuning). It also
  commits staged thresholds, but only if they were actually SET since the last
  save (dirty flag), so a mode/tuning-only save does not clobber thresholds.
- **Deadzone (ID 7) GET/SET asymmetry fixed.** The original returned a 16-bit
  `[hi=0, lo=val]` on GET but read only 8 bits on SET, so a VIA `range` could
  not round-trip it. Both sides are now a single 8-bit byte.

---

## Probing (hidapi)

Tooling venv already exists at `/tmp/viahid` (Python + `hid` package):

```sh
python3 -m venv /tmp/viahid && /tmp/viahid/bin/pip install hid   # if absent
/tmp/viahid/bin/python /tmp/via_probe.py                          # GET sweep
/tmp/viahid/bin/python /tmp/via_console.py                        # read QMK console
```

Helper scripts in `/tmp`: `via_probe.py`, `via_raw.py`, `via_keymap.py`,
`via_he_map.py`, `via_he_inspect.py`, `via_he_restore.py`, `via_he_commit.py`,
`via_console.py`.

---

## Gotchas

- **Raw HID response buffer is NOT cleared between commands.** For
  unhandled/write-only IDs, the 32-byte echo may contain **stale bytes** from
  the previous report. **Trust the console `uprintf`, not the raw echo.**
- **Excessive probing causes EEPROM wear** ("EEPROM wear!!!" printed). Each
  commit re-writes flash. Restore defaults after probing: ID 1=50, ID 2=30,
  ID 6=0, ID 7=15, ID 8=10, ID 9=10.
- **VIA definitions JSON is v3 (non-legacy)**: top-level keys
  `name, vendorId, productId, matrix, keycodes, customKeycodes, menus, layouts`
  (NO `lighting`, NO `customMenus`, NO `firmwareVersion`-required). Mixing v2
  legacy keys triggers an ajv "additional properties" error. Menu schema:
  `menus: [{label, content:[{label, content:[{type, label, content:[name, channel, valueId], options?}]}]}]`;
  `showIf` uses pelpi syntax `"{id_name} == N"` / `"{id_x.0} == 254"`.
  The definitions JSON lives at `../icebreaker_HE_via_definitions.json`
  (parent `icebreaker/` folder), loaded via VIA **Design → Load Draft Definition**.
