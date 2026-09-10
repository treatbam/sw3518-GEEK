#!/usr/bin/env python3
"""ESP32-S3-GEEK case — stock dongle silhouette + haptic belly.
Reads case_meta.json as the lock (dimensions). Writes STLs + refreshed print_outer.
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
LOCK = OUT / "case_meta.json"

DEFAULTS = {
    "version": 2,
    "stock_ref_mm": {"body": [45.00, 24.50, 9.00], "screen": [25.90, 15.86], "screen_from_far": 9.60},
    "params_mm": {
        "wall": 1.4,
        "gap": 0.30,
        "corner_r": 2.2,
        "extra_belly": 3.2,
        "floor": 1.5,
        "bottom_h": 7.4,
        "haptic_d": 10.2,
        "haptic_depth": 2.8,
        "usb_nose": 12.5,
        "usb_w": 12.2,
        "usb_h": 4.6,
    },
    "omitted": ["light_pipes", "button_rods", "button_caps"],
}


def load_lock():
    data = json.loads(json.dumps(DEFAULTS))
    if LOCK.exists():
        saved = json.loads(LOCK.read_text())
        data["version"] = saved.get("version", data["version"])
        data["stock_ref_mm"].update(saved.get("stock_ref_mm") or {})
        data["params_mm"].update(saved.get("params_mm") or {})
        if "omitted" in saved:
            data["omitted"] = saved["omitted"]
    return data


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


def cyl_at(r, h, center, axis="z"):
    m = trimesh.creation.cylinder(radius=r, height=h, sections=48)
    if axis == "y":
        m.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
    m.apply_translation(center)
    return m


def diff(a, *bs):
    out = a
    for b in bs:
        out = trimesh.boolean.difference([out, b], engine="manifold", check_volume=False)
        if isinstance(out, list):
            out = trimesh.util.concatenate(out)
    return out


def union(*ms):
    out = ms[0]
    for m in ms[1:]:
        out = trimesh.boolean.union([out, m], engine="manifold", check_volume=False)
        if isinstance(out, list):
            out = trimesh.util.concatenate(out)
    return out


def main():
    lock = load_lock()
    body = lock["stock_ref_mm"]["body"]
    screen = lock["stock_ref_mm"]["screen"]
    scr_from_far = float(lock["stock_ref_mm"]["screen_from_far"])
    p = lock["params_mm"]
    body_l, body_w, body_h = float(body[0]), float(body[1]), float(body[2])
    scr_l, scr_w = float(screen[0]), float(screen[1])
    wall, gap, corner_r = float(p["wall"]), float(p["gap"]), float(p["corner_r"])
    extra_belly = float(p["extra_belly"])
    floor = float(p["floor"])
    bottom_h = float(p["bottom_h"])
    hap_d, hap_depth = float(p["haptic_d"]), float(p["haptic_depth"])
    usb_nose, usb_w, usb_h = float(p["usb_nose"]), float(p["usb_w"]), float(p["usb_h"])

    shell_h = body_h + extra_belly
    lid_h = shell_h - bottom_h
    usb_z = floor + 1.4
    cav_l = body_l + 2 * gap
    cav_w = body_w + 2 * gap
    outer_l = cav_l + 2 * wall
    outer_w = cav_w + 2 * wall

    outer_poly = translate(round_rect(outer_l, outer_w, corner_r), xoff=outer_l / 2, yoff=0)
    bottom_outer = extrude(outer_poly, bottom_h, 0)
    cav_poly = translate(round_rect(cav_l, cav_w, max(0.6, corner_r - 0.4)),
                         xoff=wall + cav_l / 2, yoff=0)
    cavity = extrude(cav_poly, bottom_h - floor + 0.4, floor)
    usb = box_at([wall + 4, usb_w, usb_h], [wall / 2 + 1, 0, usb_z + usb_h / 2])
    usb_out = box_at([usb_nose + 2, usb_w + 0.6, usb_h + 0.6],
                     [-usb_nose / 2 + 1, 0, usb_z + usb_h / 2])
    haptic = cyl_at(hap_d / 2, hap_depth + 1.0,
                    [wall + cav_l * 0.55, 0, floor - hap_depth / 2 + 0.2])
    tf = box_at([11.5, wall + 4, 1.8],
                [wall + body_l - scr_from_far - scr_l / 2, -outer_w / 2, floor + 2.0])
    hdr = box_at([28.0, wall + 4, 3.2], [wall + 22, outer_w / 2, floor + 3.4])
    boot = cyl_at(2.0, wall + 6, [wall + cav_l - 6.0, -outer_w / 2, floor + 3.2], axis="y")
    wire = box_at([wall + 4, 7.0, 3.0], [outer_l - wall / 2, 0, floor + 3.5])
    bottom = diff(bottom_outer, cavity, usb, usb_out, haptic, tf, hdr, boot, wire)
    rail_w, rail_h = 1.6, 1.0
    rail_y = cav_w / 2 - rail_w / 2 - 0.3
    rail_z = floor + 1.0
    bottom = union(
        bottom,
        box_at([cav_l - 4, rail_w, rail_h], [wall + cav_l / 2, -rail_y, rail_z + rail_h / 2]),
        box_at([cav_l - 4, rail_w, rail_h], [wall + cav_l / 2, rail_y, rail_z + rail_h / 2]),
    )

    lid_outer = extrude(outer_poly, lid_h, 0)
    scr_cx = wall + (body_l - scr_from_far) - scr_l / 2
    lid = diff(
        lid_outer,
        box_at([scr_l + 0.4, scr_w + 0.4, lid_h + 2], [scr_cx, 0.0, lid_h / 2]),
        box_at([scr_l + 1.6, scr_w + 1.6, 0.8], [scr_cx, 0.0, lid_h - 0.3]),
        box_at([2.2, 8.0, 1.4], [1.1, 0, lid_h - 0.3]),
    )
    lip_h = 1.2
    lip = extrude(translate(round_rect(cav_l - 1.0, cav_w - 1.0, max(0.5, corner_r - 0.7)),
                            xoff=wall + cav_l / 2, yoff=0), lip_h, -lip_h + 0.15)
    lip_cut = extrude(translate(round_rect(cav_l - 2.4, cav_w - 2.4, max(0.4, corner_r - 1.1)),
                                xoff=wall + cav_l / 2, yoff=0), lip_h + 0.4, -lip_h)
    lid = union(lid, diff(lip, lip_cut))

    lid_print = lid.copy()
    T = np.eye(4)
    T[2, 2] = -1
    T[2, 3] = lid.bounds[0][2] + lid.bounds[1][2]
    lid_print.apply_transform(T)
    lid_print.apply_translation([0, 0, -lid_print.bounds[0][2]])
    bottom.apply_translation([0, 0, -bottom.bounds[0][2]])

    bottom.export(OUT / "geek_case_bottom.stl")
    lid_print.export(OUT / "geek_case_lid.stl")
    prev = lid.copy()
    prev.apply_translation([0, 0, bottom_h + 6])
    union(bottom.copy(), prev).export(OUT / "geek_case_preview_exploded.stl")
    lock["print_outer_mm"] = {
        "body": [round(outer_l, 2), round(outer_w, 2), round(shell_h, 2)],
        "haptic_d": hap_d,
    }
    LOCK.write_text(json.dumps(lock, indent=2) + "\n")
    print("exported", OUT)


if __name__ == "__main__":
    main()
