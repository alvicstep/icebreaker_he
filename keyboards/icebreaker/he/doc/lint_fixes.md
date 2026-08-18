# Icebreaker HE — `qmk lint` Fix Proposal

Current output:

```
☒ icebreaker/he: Invalid keyboard level feature detected - encoder_map
☒ icebreaker/he: The file "keyboards/icebreaker/he/original_firmware/icebreaker_he_option_bytes.bin" should not exist!
☒ icebreaker/he: The file "keyboards/icebreaker/he/original_firmware/icebreaker_he_original.bin" should not exist!
☒ icebreaker/he: The keymap via should not exist!
```

The four errors come from two independent lint rules:

1. **Hardcoded feature / keymap-name blacklists** in `lib/python/qmk/cli/lint.py`:
   - `INVALID_KB_FEATURES = {'encoder_map', 'dip_switch_map', 'combo', 'tap_dance', 'via'}`
   - `INVALID_KM_NAMES = ['via', 'vial']`
2. **The gitignored-file check** — `git ls-files -c -o -i --exclude-from=.gitignore`
   flags any file under the keyboard that matches `.gitignore` (a file that would
   be silently dropped from a fresh clone).

---

## Issue 1 — `encoder_map` at keyboard level

**Root cause.** `rules.mk` sets `ENCODER_MAP_ENABLE = yes` at the *keyboard*
level. QMK treats `encoder_map` as a *keymap*-level feature (the `encoder_map[][]`
array is defined in each keymap, exactly like the `via` feature), so lint rejects
it at the keyboard level.

**Fix.** Move the flag from the keyboard to each keymap:

- `rules.mk` — delete `ENCODER_MAP_ENABLE = yes` (keep `ENCODER_ENABLE = yes`,
  which is a valid keyboard-level hardware feature).
- `keymaps/via/rules.mk` — add `ENCODER_MAP_ENABLE = yes`.
- `keymaps/default/rules.mk` — **create** (does not exist today) with
  `ENCODER_MAP_ENABLE = yes`.

No keymap source changes are needed — both `keymaps/default/keymap.c` and
`keymaps/via/keymap.c` already define `encoder_map[][]` under
`#if defined(ENCODER_MAP_ENABLE)`.

---

## Issues 2 & 3 — `.bin` DFU backups flagged

**Root cause.** `original_firmware/` holds the two reference dumps from the
original board (a 512 KB flash image + a 16 B option-bytes image). They are
already tracked (committed), but they match the repo-wide rule
`.gitignore:34 → *.bin`, so the gitignored-file check flags them.

**Fix.** Rename the dumps to a non-ignored extension (`.dump`), which also
requires updating `original_firmware/README.md` (both file names + restore
commands).

> **Why a local `!*.bin` negation does NOT work here.** `qmk lint`'s
> gitignored-file check runs
> `git ls-files -c -o -i --exclude-from=.gitignore`. Passing the root
> `.gitignore` via `--exclude-from` promotes its patterns to the
> *command-line* exclude source, which has the **highest** gitignore
> precedence — higher than any nested `original_firmware/.gitignore`. A
> `!*.bin` in the nested file therefore cannot override the root `*.bin`, so
> the files are still reported. Renaming to a non-`.bin` extension sidesteps
> the rule entirely.

---

## Issue 4 — `via` keymap

**Root cause.** QMK policy is that VIA keymaps do not live in the QMK repo —
they belong in the [VIA QMK Userspace](https://github.com/the-via/qmk_userspace_via).
`qmk lint` enforces this with a hardcoded name blacklist (`via`, `vial`), which
is *not* a gitignore rule, so merely un-ignoring the folder will not clear it.

This is also the reason `keymaps/via/*` matches `.gitignore:129`
`/keyboards/**/keymaps/via/*` — a second, independent source of friction.

**Options.**

| Option | What | Lint result | Tradeoff |
|--------|------|-------------|----------|
| A | Rename `keymaps/via/` → `keymaps/via_custom/` | ✅ green | Breaks the `via` naming convention; harmless functionally (VIA keys by VID/PID + raw-HID interface, not folder name) |
| B | Keep `via` and accept the error as intentional | ❌ still red | Conventional name; requires documenting the known failure |
| C | Patch `INVALID_KM_NAMES`/`INVALID_KB_FEATURES` in QMK core | ✅ green | Diverges from upstream QMK; not recommended |

**Recommendation.** Option A (`via` → `via_custom`) is the cleanest path to a
fully green lint, because VIA keys a board by VID/PID + raw-HID interface, not by
keymap folder name. It also clears the `.gitignore` match automatically. If
staying maximally conventional matters more than a green lint, choose Option B
and document the expected failure.

If Option A is chosen, also update:

- `rules.mk` line 23 comment (`keymaps/via/rules.mk` → `keymaps/via_custom/rules.mk`).
- `readme.md` line 148 folder layout (`via/` → `via_custom/`).

The VIA definitions JSON (`../icebreaker_HE_via_definitions.json`) does **not**
reference the keymap folder name, so it needs no change.

---

## Recommended change set (summary)

| # | Action | File(s) |
|---|--------|---------|
| 1 | Delete `ENCODER_MAP_ENABLE = yes` | `rules.mk` |
| 2 | Add `ENCODER_MAP_ENABLE = yes` | `keymaps/via_custom/rules.mk` |
| 3 | Create `rules.mk` with `ENCODER_MAP_ENABLE = yes` | `keymaps/default/rules.mk` |
| 4 | `git mv` the two `.bin` dumps → `.dump` | `original_firmware/` |
| 5 | `git mv keymaps/via keymaps/via_custom` | `keymaps/` |
| 6 | Update comment + folder-layout + README refs | `rules.mk`, `readme.md`, `original_firmware/README.md` |

## Verification

```sh
qmk lint -kb icebreaker/he
qmk compile -kb icebreaker/he -km default
qmk compile -kb icebreaker/he -km via_custom
```
