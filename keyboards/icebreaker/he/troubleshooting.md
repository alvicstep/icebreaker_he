# Icebreaker HE — bring-up notes & troubleshooting log

> Field notes from reverse-engineering the Serene Industries "Icebreaker HE"
> (Hall-effect) keyboard onto a clean-room QMK driver. Each entry documents a
> symptom that was observed on the **live board**, the root cause, and the fix.
> This is deliberately written for the next person who has to debug this board
> (or any STM32F411 + ChibiOS OTG + Hall-effect matrix), so the hard-won facts
> are not lost.

---

## 1. USB never enumerates (no device appears at all)

**Symptom.** Firmware boots (the boot trace shows the main loop running), but
the host never sees a USB device — no "Icebreaker HE" in `lsusb`, no HID
interfaces, nothing. Everything else is fine.

**Root cause.** The STM32F411's OTG is a *step-1* OTG core. ChibiOS' OTGv1 LLDs
has a `STM32_OTG_STEPPING == 1` code path whose `usb_lld_connect_bus()`
implemented the connect mechanism as a `GCCFG_VBUSBSEN` write. That bit is the
F446-class VBUS-sensing connect; on a board that declares
`BOARD_OTG_NOVBUSSENS` (no VBUS sense hardware — this one is powered and its
pull-up is a fixed resistor), toggling `GCCFG_VBUSBSEN` does **nothing** to the
`D+` line. The device therefore never pulls `D+` up, the host never sees a
connection, and enumeration never starts.

**Fix.** Connect/disconnect the bus via the **soft-disconnect** bit in the
device control register instead:

- `lib/chibios/os/hal/ports/STM32/LLD/OTGv1/hal_usb_lld.h`
  - `usb_lld_connect_bus(usbp)` → `(usbp)->otg->DCTL &= ~DCTL_SDIS`
  - `usb_lld_disconnect_bus(usbp)` → `(usbp)->otg->DCTL |= DCTL_SDIS`
  - (for the `BOARD_OTG_NOVBUSSENS` / stepping-1 branch)
- `lib/chibios/os/hal/ports/STM32/LLD/OTGv1/hal_usb_lld.c`
  - Guard the `GOTGCTL = GOTGCTL_BVALOEN | GOTGCTL_BVALOVAL` write in
    `usb_lld_start()` with `#if !defined(BOARD_OTG_NOVBUSSENS)`.

Both changes were verified in the disassembly of the rebuilt binary.

The exact diff is checked in as `chibios_novbussens.patch` (next to this file).
`lib/chibios` is a git submodule, so its working-tree edits are *not* captured
by the parent repo's commit. To rebuild from a fresh clone:

```sh
git submodule update --init lib/chibios
cd lib/chibios
git apply ../../keyboards/icebreaker/he/chibios_novbussens.patch
cd ../..
```

**How it was found.** The boot trace (`boot_trace.h`) showed the main loop alive
with heartbeats, but the USB-state markers never advanced past `USB_STOP`/`READY`
— the host had never reset the bus, i.e. `D+` was never pulled up. That pointed
at the connect mechanism rather than descriptors or the clock tree.

---

## 2. Boot loop straight into DFU (never reaches the app)

**Symptom.** On power-up the board jumps straight into the STM32 ROM DFU
bootloader (or resets in a loop), never running the keyboard firmware.

**Root cause.** `VIA_ENABLE = yes` forces `BOOTMAGIC_ENABLE = yes` in
`builddefs/common_features.mk`, so QMK's bootmagic scan runs during
`keyboard_init()` even though `keyboard.json` sets `"bootmagic": false`.
The bootmagic key is `[row 0, col 0]` (Esc). The Hall sensors rest near
mid-scale, and *before calibration* a spurious "pressed" reading on that key
makes `bootmagic_should_reset()` return true, which triggers
`bootmagic_reset_eeprom()` + `bootloader_jump()` on **every** power-up. The
board has no physical bootmagic key, so this is purely a false positive.

**Fix.** Override the weak decision hook in `he_matrix.c` to always decline:

```c
bool bootmagic_should_reset(void);
bool bootmagic_should_reset(void) {
    return false;
}
```

---

## 3. Enumerates correctly, but no keys register

**Symptom.** USB enumerates as "Icebreaker HE" (3 HID interfaces: keyboard +
console + VIA raw), the main loop is running, the ADC/mux hardware is confirmed
good (resting values ≈ 2150 raw, as expected for a bipolar Hall sensor at
mid-scale) — but **pressing any key does nothing**.

**Root cause — wrong calibration mapping, not wrong hardware.** The Hall sensors
are bipolar and read **higher** (ADC *up*) as the magnet approaches, which the
original defaults half-anticipated. The bug was the *span*: a full bottom-out
moves the ADC from ~2150 to ~2842 — a swing of only **~700 counts**, not the
full 4095 the uncalibrated defaults assumed.

With `raw_min = 2048` and `raw_max = 4095`, a full press maps to

```
travel = (2842 - 2048) * 100 / (4095 - 2048) = 38%
```

which is **below the 50% actuation point** — so a full press never reached the
threshold and no key ever reported pressed. The boot trace's "first pressed
sensor" slot stayed blank forever, which was the tell that this was a mapping
problem rather than a dead matrix.

**Fix.** Boot-time auto-calibration in `he_matrix.c` (see
`matrix_scan_custom`): over the first `HE_ADC_REST_SAMPLE_SCANS` scans, track
each sensor's resting floor, then map

- `raw_min = rest_floor - HE_ADC_REST_MARGIN` → resting reads ~0% travel
- `raw_max = raw_min + HE_ADC_TRAVEL_SPAN` → a full press reads ~100% travel

The tunables live in `config.h`:

| Constant | Value | Meaning |
|---|---|---|
| `HE_ADC_REST_SAMPLE_SCANS` | 64 | scans to sample the resting value before locking in |
| `HE_ADC_REST_MARGIN` | 20 | raw counts below the floor so rest maps slightly above 0% |
| `HE_ADC_TRAVEL_SPAN` | 700 | full-press swing in raw counts (measured ~692) |

The VIA calibration buttons (IDs 4/5) still override these with true per-key
min/max if a user runs a full calibration. **Keys now register.**

**Measured Hall characteristics (live board):**

| Condition | Raw ADC (12-bit) |
|---|---|
| Rest (magnet far) | ~2150 (mid-scale) |
| Full bottom-out | ~2842 |
| Swing | ~700 counts, **press = ADC up** |

> If sensitivity feels off, `HE_ADC_TRAVEL_SPAN` is the single knob to tune:
> lower = more sensitive, higher = less sensitive. It was derived from one
> manual press session, so per-key variation (actuation/release %) is still
> worth fine-tuning through VIA.

---

## 4. STM32 ROM DFU `:leave` does not reliably re-enumerate

**Symptom.** After `dfu-util ... :leave`, the board transitions to
`dfuMANIFEST` state but does **not** reset into the application — it just sits
in DFU. The freshly-flashed firmware never runs, so (a) keys do nothing and (b)
the boot-trace sector stays blank/erased (no app ever wrote to it).

**Fix / workaround.** Do a **physical USB unplug + replug** after flashing.
That forces the ROM bootloader to run → jump to the application → the app writes
its boot trace and enumerates.

**How to recognise it.** If a freshly-dumped trace is 100% `0xFF` (no stage
markers, no heartbeats, no value slots) even though `dfu-util` reported a
successful download, the app never ran — replug and retry.

---

## 5. The flash-backed boot trace (how bring-up was debugged)

The QMK HID console can't print until USB enumerates, which happens late in
boot. To localise hangs *before* enumeration, each stage writes a 32-bit marker
into flash **sector 5** (`0x08020000`..`0x0803FFFF`, 128 KB) — entirely beyond
the firmware image, so it survives across the boot that's being diagnosed.

Three regions (see `boot_trace.h` / `boot_trace.c`):

| Region | Address | Contents |
|---|---|---|
| Stage words | `0x08020000 + stage*4` | `0xAA000000 | stage` (one per stage) |
| Heartbeat | `0x08020100 + idx*4` | `0xBB000000 | idx`, main-loop liveness |
| Value slots | `0x08021000 + slot*4` | arbitrary 32-bit diagnostic values |

**Workflow:**

```sh
# 1. flash + replug, let it run (and press keys if probing input)
# 2. put the board back into DFU
# 3. dump the trace sector (131072 bytes)
rm -f icebreaker_trace.bin
dfu-util -a 0 -s 0x08020000:131072 -U icebreaker_trace.bin

# 4. decode it
python3 decode_trace.py
```

`decode_trace.py` prints the stage markers, heartbeat range, and populated value
slots. The stage enum in `boot_trace.h` maps the highest marker to how far boot
got (USB state markers `BT_USB_STOP`..`BT_USB_SUSPENDED` encode ChibiOS
`usbstate_t`, so `BT_USB_ACTIVE` present = full enumeration).

**Value-slot cheat sheet** (populated in `he_matrix.c`):

| Slot | Meaning |
|---|---|
| 0–2 | raw ADC of sensors 0 / 32 / 46 on the first scan (resting reference) |
| 3 | encoder-push GPIO level on the first scan |
| 4 | `he_scan_count` when USB reached `USB_ACTIVE` |
| 5 | `he_scan_count` when USB reached `USB_SUSPENDED` |
| 6 | **first sensor reported pressed** (blank = none ever pressed) |
| 7 | `he_scan_count` at first press |
| 8 | raw ADC at first press (`0xFFFF` = encoder push) |
| 9 / 10 | max / min raw ADC up to first suspend |
| 11 / 12 | suspend / wakeup hook call counts |
| 14 / 15 | ms timestamps at matrix init / keyboard post-init (14→15 gap = EEPROM erase) |
| 16 | ms timestamp at first matrix scan |
| 20–26 | USB event bitmask at scans 5/20/100/500/2000/1/2 |
| 27 / 28 | process stack pointer at post-init / matrix init (overflow probe) |
| 29 / 30 | global max / min raw ADC by scan 5000 (**press-polarity probe**) |
| 31 | global max raw ADC by scan 20000 (late press probe) |
| 32 / 33 | auto-calibrated `raw_min` / `raw_max` for sensor 0 (after rest window) |

> **Debugging note.** Slot 6 blank + heartbeats present = the scan loop runs but
> the press state never crosses the actuation threshold — i.e. a
> calibration/mapping problem, *not* dead hardware. Slots 29/30/31 then reveal
> the actual press polarity and magnitude (see §3).

---

## Key lessons (one-liners)

- **OTG step-1 connect = `DCTL_SDIS`, never `GCCFG_VBUSBSEN`** on a NOVBUSSENS board.
- **Bipolar Hall = ADC *up* on press**, but only a ~700-count swing at 12-bit —
  do not assume full-scale travel.
- **`dfu-util :leave` ≠ app reset** — always physically replug to run the app.
- **Resting value ≈ 2150** is *correct* (mid-scale) — it was never the bug; the
  missing span was.
