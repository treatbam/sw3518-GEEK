#!/usr/bin/env python3
"""ESP32-S3-GEEK case v2 — stock dongle silhouette + haptic belly.
No light pipes, no button rods/caps.
Requires: pip install trimesh manifold3d shapely scipy numpy
"""
from pathlib import Path
import json
import numpy as np
import trimesh
from shapely.geometry import box as sbox, Point
from shapely.ops import unary_union
from shapely.affinity import translate

OUT = Path(__file__).resolve().parent
BODY_L, BODY_W = 45.00, 24.50
SCR_L, SCR_W, SCR_FROM_FAR = 25.90, 15.86, 9.60
WALL, GAP, CORNER_R = 1.4, 0.30, 2.2
SHELL_H = 9.0 + 3.2
BOTTOM_H = 7.4
LID_H = SHELL_H - BOTTOM_H
FLOOR = 1.5
USB_NOSE, USB_W, USB_H = 12.5, 12.2, 4.6
USB_Z = FLOOR + 1.4
CAV_L = BODY_L + 2 * GAP
CAV_W = BODY_W + 2 * GAP
OUTER_L = CAV_L + 2 * WALL
OUTER_W = CAV_W + 2 * WALL
HAP_D, HAP_DEPTH = 10.2, 2.8


def round_rect(length, width, radius):
    l2, w2 = length / 2, width / 2
    r = min(radius, l2 - 0.05, w2 - 0.05)
    return unary_union([
        sbox(-l2 + r, -w2 + r, l2 - r, w2 - r),
        sbox(-l2 + r, -w2, l2 - r, w2),
        sbox(-l2, -w2 + r, l2, w2 - r),
        Point(-l2 + r, -w2 + r).buffer(r),
        Point(l2 - r, -w2 + r).buffer(r),
        Point(-l2 + r, w2 - r).buffer(r),
        Point(l2 - r, w2 - r).buffer(r),
    ])


def extrude(poly, height, z0=0.0):
    mesh = trimesh.creation.extrude_polygon(poly, height=height)
    mesh.apply_translation([0, 0, z0])
    return mesh


def box_at(extents, center):
    m = trimesh.creation.box(extents=extents)
    m.apply_translation(center)
    return m


def cyl_at(r, h, center, axis='z'):
    m = trimesh.creation.cylinder(radius=r, height=h, sections=48)
    if axis == 'y':
        m.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
    m.apply_translation(center)
    return m


def diff(a, *bs):
    out = a
    for b in bs:
        out = trimesh.boolean.difference([out, b], engine='manifold', check_volume=False)
        if isinstance(out, list):
            out = trimesh.util.concatenate(out)
    return out


def union(*ms):
    out = ms[0]
    for m in ms[1:]:
        out = trimesh.boolean.union([out, m], engine='manifold', check_volume=False)
        if isinstance(out, list):
            out = trimesh.util.concatenate(out)
    return out


def main():
    outer_poly = translate(round_rect(OUTER_L, OUTER_W, CORNER_R), xoff=OUTER_L / 2, yoff=0)
    bottom_outer = extrude(outer_poly, BOTTOM_H, 0)
    cav_poly = translate(round_rect(CAV_L, CAV_W, max(0.6, CORNER_R - 0.4)),
                         xoff=WALL + CAV_L / 2, yoff=0)
    cavity = extrude(cav_poly, BOTTOM_H - FLOOR + 0.4, FLOOR)
    usb = box_at([WALL + 4, USB_W, USB_H], [WALL / 2 + 1, 0, USB_Z + USB_H / 2])
    usb_out = box_at([USB_NOSE + 2, USB_W + 0.6, USB_H + 0.6],
                     [-USB_NOSE / 2 + 1, 0, USB_Z + USB_H / 2])
    haptic = cyl_at(HAP_D / 2, HAP_DEPTH + 1.0,
                    [WALL + CAV_L * 0.55, 0, FLOOR - HAP_DEPTH / 2 + 0.2])
    tf = box_at([11.5, WALL + 4, 1.8],
                [WALL + BODY_L - SCR_FROM_FAR - SCR_L / 2, -OUTER_W / 2, FLOOR + 2.0])
    hdr = box_at([28.0, WALL + 4, 3.2], [WALL + 22, OUTER_W / 2, FLOOR + 3.4])
    boot = cyl_at(2.0, WALL + 6, [WALL + CAV_L - 6.0, -OUTER_W / 2, FLOOR + 3.2], axis='y')
    wire = box_at([WALL + 4, 7.0, 3.0], [OUTER_L - WALL / 2, 0, FLOOR + 3.5])
    bottom = diff(bottom_outer, cavity, usb, usb_out, haptic, tf, hdr, boot, wire)
    rail_w, rail_h = 1.6, 1.0
    rail_y = CAV_W / 2 - rail_w / 2 - 0.3
    rail_z = FLOOR + 1.0
    bottom = union(
        bottom,
        box_at([CAV_L - 4, rail_w, rail_h], [WALL + CAV_L / 2, -rail_y, rail_z + rail_h / 2]),
        box_at([CAV_L - 4, rail_w, rail_h], [WALL + CAV_L / 2, rail_y, rail_z + rail_h / 2]),
    )

    lid_outer = extrude(outer_poly, LID_H, 0)
    scr_cx = WALL + (BODY_L - SCR_FROM_FAR) - SCR_L / 2
    lid = diff(
        lid_outer,
        box_at([SCR_L + 0.4, SCR_W + 0.4, LID_H + 2], [scr_cx, 0.0, LID_H / 2]),
        box_at([SCR_L + 1.6, SCR_W + 1.6, 0.8], [scr_cx, 0.0, LID_H - 0.3]),
        box_at([2.2, 8.0, 1.4], [1.1, 0, LID_H - 0.3]),
    )
    lip_h = 1.2
    lip = extrude(translate(round_rect(CAV_L - 1.0, CAV_W - 1.0, max(0.5, CORNER_R - 0.7)),
                            xoff=WALL + CAV_L / 2, yoff=0), lip_h, -lip_h + 0.15)
    lip_cut = extrude(translate(round_rect(CAV_L - 2.4, CAV_W - 2.4, max(0.4, CORNER_R - 1.1)),
                                xoff=WALL + CAV_L / 2, yoff=0), lip_h + 0.4, -lip_h)
    lid = union(lid, diff(lip, lip_cut))

    lid_print = lid.copy()
    T = np.eye(4)
    T[2, 2] = -1
    T[2, 3] = lid.bounds[0][2] + lid.bounds[1][2]
    lid_print.apply_transform(T)
    lid_print.apply_translation([0, 0, -lid_print.bounds[0][2]])
    bottom.apply_translation([0, 0, -bottom.bounds[0][2]])

    bottom.export(OUT / 'geek_case_bottom.stl')
    lid_print.export(OUT / 'geek_case_lid.stl')
    prev = lid.copy()
    prev.apply_translation([0, 0, BOTTOM_H + 6])
    union(bottom.copy(), prev).export(OUT / 'geek_case_preview_exploded.stl')
    (OUT / 'case_meta.json').write_text(json.dumps({
        'version': 2,
        'stock_ref_mm': {'body': [45, 24.5, 9], 'screen': [25.9, 15.86], 'screen_from_far': 9.6},
        'print_outer_mm': {'body': [round(OUTER_L, 2), round(OUTER_W, 2), round(SHELL_H, 2)],
                           'haptic_d': HAP_D},
        'omitted': ['light_pipes', 'button_rods', 'button_caps'],
    }, indent=2))
    print('exported', OUT)


if __name__ == '__main__':
    main()
