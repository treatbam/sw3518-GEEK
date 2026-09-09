#!/usr/bin/env python3
"""Parametric ESP32-S3-GEEK case for sw3518-GEEK. Requires trimesh + manifold."""
from pathlib import Path
import json
import numpy as np
import trimesh
from trimesh.creation import box, cylinder

OUT = Path(__file__).resolve().parent

BOARD_L, BOARD_W, BOARD_H = 61.0, 24.5, 7.2
WALL = 1.6
GAP = 0.35
FLOOR = 1.8
LID = 1.6
INNER_H = BOARD_H + 1.2
USB_PROTRUDE = 11.0
USB_W, USB_H = 13.0, 5.0
SCR_L, SCR_W = 26.5, 16.0
SCR_X = 18.0
SCR_Y = (BOARD_W - SCR_W) / 2
BTN_D = 4.2
BTN_Z = FLOOR + 3.5
BTN_X = 30.0
HAP_D, HAP_H = 10.5, 3.2
HAP_X, HAP_Y = 38.0, BOARD_W / 2
LED_XY = 5.4
LED_X, LED_Y = 8.0, BOARD_W / 2
BOOT_D = 5.0
BOOT_X, BOOT_Y = 8.0, BOARD_W / 2
TF_L, TF_H = 12.0, 2.2
TF_X = 40.0
WIRE_W, WIRE_H = 8.0, 3.5

INNER_L = BOARD_L + 2 * GAP
INNER_W = BOARD_W + 2 * GAP
USB_OPEN = 9.5
OUTER_L = INNER_L + 2 * WALL + USB_OPEN
OUTER_W = INNER_W + 2 * WALL + 6.0
CAV_X0 = WALL + USB_OPEN
CAV_Y0 = (OUTER_W - INNER_W) / 2


def solid_box(extents, center):
    m = box(extents=extents)
    m.apply_translation(center)
    return m


def cyl(radius, height, center, axis='z'):
    m = cylinder(radius=radius, height=height, sections=48)
    if axis == 'x':
        m.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [0, 1, 0]))
    elif axis == 'y':
        m.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
    m.apply_translation(center)
    return m


def diff(a, *bs):
    out = a
    for b in bs:
        out = out.difference(b, engine='manifold')
        if isinstance(out, list):
            out = trimesh.util.concatenate(out)
    return out


def union(*ms):
    out = ms[0]
    for m in ms[1:]:
        out = out.union(m, engine='manifold')
        if isinstance(out, list):
            out = trimesh.util.concatenate(out)
    return out


def main():
    outer = solid_box([OUTER_L, OUTER_W, FLOOR + INNER_H],
                      [OUTER_L / 2, OUTER_W / 2, (FLOOR + INNER_H) / 2])
    cavity = solid_box([INNER_L, INNER_W, INNER_H + 0.2],
                       [CAV_X0 + INNER_L / 2, CAV_Y0 + INNER_W / 2, FLOOR + (INNER_H + 0.2) / 2])
    usb_tunnel = solid_box([USB_OPEN + WALL + 2, USB_W, USB_H],
                           [USB_OPEN / 2, OUTER_W / 2, FLOOR + 1.5 + USB_H / 2])
    btn_left = cyl(BTN_D / 2, WALL + 8, [CAV_X0 + BTN_X, CAV_Y0 - 1, BTN_Z], axis='y')
    btn_right = cyl(BTN_D / 2, WALL + 8, [CAV_X0 + BTN_X, CAV_Y0 + INNER_W + 1, BTN_Z], axis='y')
    haptic = cyl(HAP_D / 2, HAP_H + 0.4,
                 [CAV_X0 + HAP_X, CAV_Y0 + HAP_Y, FLOOR - HAP_H / 2 + 0.2], axis='z')
    boot = cyl(BOOT_D / 2, FLOOR + 2, [CAV_X0 + BOOT_X, CAV_Y0 + BOOT_Y, FLOOR / 2], axis='z')
    tf = solid_box([TF_L, WALL + 4, TF_H],
                   [CAV_X0 + TF_X, WALL / 2, FLOOR + 2.0 + TF_H / 2])
    wire = solid_box([WALL + 4, WIRE_W, WIRE_H],
                     [OUTER_L - WALL / 2, OUTER_W / 2, FLOOR + 2.0 + WIRE_H / 2])

    def post(x, y):
        return solid_box([2.2, 2.2, 1.2], [x, y, FLOOR + 0.6])

    posts = union(
        post(CAV_X0 + 3, CAV_Y0 + 3),
        post(CAV_X0 + INNER_L - 3, CAV_Y0 + 3),
        post(CAV_X0 + 3, CAV_Y0 + INNER_W - 3),
        post(CAV_X0 + INNER_L - 3, CAV_Y0 + INNER_W - 3),
    )

    bottom = diff(outer, cavity, usb_tunnel, btn_left, btn_right, haptic, boot, tf, wire)
    bottom = union(bottom, posts)
    ledge = solid_box([INNER_L - 0.6, INNER_W - 0.6, 0.9],
                      [CAV_X0 + INNER_L / 2, CAV_Y0 + INNER_W / 2, FLOOR + INNER_H - 0.35])
    ledge_cut = solid_box([INNER_L - 2.2, INNER_W - 2.2, 1.2],
                          [CAV_X0 + INNER_L / 2, CAV_Y0 + INNER_W / 2, FLOOR + INNER_H - 0.3])
    bottom = union(bottom, diff(ledge, ledge_cut))

    lid_outer = solid_box([OUTER_L, OUTER_W, LID], [OUTER_L / 2, OUTER_W / 2, LID / 2])
    scr = solid_box([SCR_L, SCR_W, LID + 2],
                    [CAV_X0 + SCR_X + SCR_L / 2, CAV_Y0 + SCR_Y + SCR_W / 2, LID / 2])
    led = solid_box([LED_XY, LED_XY, LID + 2], [CAV_X0 + LED_X, CAV_Y0 + LED_Y, LID / 2])
    lip = solid_box([INNER_L - 1.0, INNER_W - 1.0, 1.4],
                    [CAV_X0 + INNER_L / 2, CAV_Y0 + INNER_W / 2, -0.5])
    lip_cut = solid_box([INNER_L - 2.6, INNER_W - 2.6, 2.0],
                        [CAV_X0 + INNER_L / 2, CAV_Y0 + INNER_W / 2, -0.5])
    lid = union(diff(lid_outer, scr, led), diff(lip, lip_cut))
    lid.apply_translation([0, 0, 1.4])
    T = np.eye(4)
    T[2, 2] = -1
    T[2, 3] = lid.bounds[1][2] + lid.bounds[0][2]
    lid_print = lid.copy()
    lid_print.apply_transform(T)

    cap = union(cyl(3.0, 2.5, [0, 0, 1.25]), cyl(1.4, 3.5, [0, 0, 0.5]))

    bottom.export(OUT / 'geek_case_bottom.stl')
    lid_print.export(OUT / 'geek_case_lid.stl')
    cap.export(OUT / 'geek_btn_cap.stl')
    prev_l = lid.copy()
    prev_l.apply_translation([0, 0, FLOOR + INNER_H + 8])
    union(bottom.copy(), prev_l).export(OUT / 'geek_case_preview_exploded.stl')
    (OUT / 'case_meta.json').write_text(json.dumps({
        'board_mm': [BOARD_L, BOARD_W, BOARD_H],
        'outer_mm': [round(OUTER_L, 2), round(OUTER_W, 2), round(FLOOR + INNER_H, 2)],
        'pins': {'BTN_LEFT': 1, 'BTN_RIGHT': 2, 'HAPTIC': 13, 'WS2812_suggest': 14},
    }, indent=2))
    print('exported to', OUT)


if __name__ == '__main__':
    main()
