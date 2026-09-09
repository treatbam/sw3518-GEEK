# ESP32-S3-GEEK case (sw3518-GEEK)

Printable shell sized for the Waveshare **ESP32-S3-GEEK** (`61.00 × 24.50` mm) with room for:

- Side tact buttons (GPIO `1` / `2`)
- Coin haptic motor pocket (~10 mm) under the board (drive with NPN/FET on GPIO `13`)
- WS2812B light-pipe hole in the lid (suggest GPIO `14`, 5 V power)
- USB-A open end, screen window, BOOT finger hole, TF slot, I2C wire exit toward SW3518

## Files

| File | What |
|------|------|
| `geek_case_bottom.stl` | Main shell |
| `geek_case_lid.stl` | Lid (print outer face down) |
| `geek_btn_cap.stl` | Optional stem cap ×2 |
| `geek_case_preview_exploded.stl` | Visual only — do not print |
| `generate_case.py` | Parametric generator (tweak + re-export) |

## Print

- Material: PLA for fit checks, PETG for daily use
- 0.2 mm layers, 3 walls, 15–20% infill
- Bottom: flat on bed. Lid: outer skin down (already oriented)
- Supports usually not required

## Fit notes

Screen window / button / haptic positions are **approximate** from the published outline + typical GEEK layout. Dry-fit the board before locking a final print; edit constants at the top of `generate_case.py` and re-run:

```bash
python3 generate_case.py
```

(Requires `trimesh` + manifold — see script header.)

## Wiring clearance

Leave the far-end wire notch free for the 4-pin I2C lead to the SW3518. Do not bury BOOT — the underside hole is for recovery flashes.
