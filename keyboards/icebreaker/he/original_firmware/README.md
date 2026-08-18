# Icebreaker HE — Original Firmware Backup

Full flash + option-bytes dump of the **Serene Industries Icebreaker HE** (Hall Effect)
board, taken over STM32 DFU while the board was in bootloader mode.

- **MCU**: STM32F411 (512 KB internal flash @ `0x08000000`)
- **DFU device**: `[0483:df11]` (STM32 ROM bootloader)

## Files

| File | Size | SHA-256 |
|------|------|---------|
| `icebreaker_he_original.bin` | 524288 B | `5000be8cd361276b5a34122891052c6138ca0323995b23ed3eaeefd5f9035e86` |
| `icebreaker_he_option_bytes.bin` | 16 B | `d292558017cf9ca0a2e40e262a5c1daa4b305ccf084ce06128133d282f905115` |

## Restoring the original firmware

Put the board in DFU mode (layer 2 → "Firmware Update" key, or the boot/reset
method), then flash the full image back:

```sh
dfu-util -a 0 -s 0x08000000:leave -D keyboards/icebreaker/he/original_firmware/icebreaker_he_original.bin
```

`-s ...:leave` resets the board out of DFU after flashing, so it reboots into
the restored firmware automatically.

## Restoring option bytes (only if they were ever changed)

```sh
dfu-util -a 1 -s 0x1FFFC000 -D keyboards/icebreaker/he/original_firmware/icebreaker_he_option_bytes.bin
```

> ⚠️ Only touch the option bytes if the board is truly bricked by a bad
> read-protection (RDP) setting. A normal failed firmware build never modifies
> option bytes — restoring the main flash image above is almost always enough.

## Exit DFU without flashing

Unplug / replug USB (or press reset once).
