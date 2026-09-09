# ESP32-S3-GEEK case (v2)

Stock-faithful USB-dongle shell from the Waveshare size drawing, with a slightly taller belly for a ~10 mm coin haptic. **No light pipes, no button rods/caps.**

## Print

| File | Notes |
|------|--------|
| `geek_case_bottom.stl` | Main shell (~48.4 × 27.9 × 7.4 mm) |
| `geek_case_lid.stl` | Lid with screen window + bezel (print outer face down) |
| `geek_case_preview_exploded.stl` | Visual only |

## Stock reference (mm)

- Body `45.00 × 24.50 × 9.00`
- Screen window `25.90 × 15.86`, `9.60` from far (non-USB) end
- Overall with USB cap `61.00`

This print is ~`+3.2` mm taller for the haptic pocket and ~`+1.4` mm walls around the board.

## Features

- Rounded corners
- USB-A open nose
- Screen window aligned to the drawing + shallow bezel
- TF slot, BOOT side hole, header strip notch, far-end wire relief
- Under-PCB haptic pocket + side rails so the board sits above the motor
- Friction lip lid

## Print settings

PLA for fit check, PETG for daily · 0.2 mm · 3 walls · 15–20% · supports usually off

Tune `GAP` / `EXTRA_BELLY` / `HAP_D` in `generate_case.py` after a dry-fit.
