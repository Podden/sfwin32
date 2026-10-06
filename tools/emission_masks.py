"""Emission masks: which texels of a ship or station texture are lights.

    uv run --with numpy --with scipy --with pillow python tools/emission_masks.py [--dump dump_v2] [--hd hd_v2] [--sheet out.png]

fightport (--fx glow) lets the texels a mask marks shine at full brightness,
keep them longer in the fog and feed them to the bloom: lit windows, lamps
and panels on ships and stations. A mask is <hd>/emit/<data>.png, named by
the texture's data hash so that it covers every palette, at the original's
256x256 (the GPU interpolates it onto the HD texture).

Lights are found in the dumped originals (<dump>/tex/*.tga, see --dump in
fightport): small, bright spots, yellow-white or pale, clearly brighter than
what surrounds them. Hazard stripes, painted emblems and the like are bright
too but large, and are dropped by size; highlights on gold plating sit in
bright paint, not in a dark hull, and are dropped by their surroundings.
Seabed atlases get no mask. Everything is derived from the user's own dump; nothing of the game's
is part of this file.
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

MAX_AREA = 48      # texels at 256x256: a window or a lamp, not a stripe
MAX_SIDE = 14
MIN_LIFT = 0.15    # brighter than the 9x9 mean around it by this much
MAX_RING = 0.35    # mean luminance of what is around it, other lights aside


def mask(rgba: np.ndarray) -> np.ndarray:
    a = rgba[..., :3].astype(np.float32) / 255.0
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mx, mn = a.max(-1), a.min(-1)
    lum = a @ np.array([0.30, 0.59, 0.11], np.float32)
    warm = (r >= 0.9 * g) & (g > 0.45) & (b < 0.8 * g)
    pale = (mn > 0.45 * mx) & (mx > 0.80)
    cand = (mx > 0.70) & (warm | pale) & (rgba[..., 3] > 127)
    cand &= lum - ndimage.uniform_filter(lum, 9) > MIN_LIFT
    # 4-connected: the squares of a lit checker panel are lights each
    lab, n = ndimage.label(cand)
    keep = np.zeros(n + 1, bool)
    for i, sl in enumerate(ndimage.find_objects(lab), 1):
        h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        if max(h, w) > MAX_SIDE or (lab[sl] == i).sum() > MAX_AREA:
            continue
        # A light sits in a dark hull; a highlight on gold plating or a
        # piece of a hazard stripe sits in more of the same bright paint.
        y0, y1 = max(sl[0].start - 2, 0), sl[0].stop + 2
        x0, x1 = max(sl[1].start - 2, 0), sl[1].stop + 2
        own = lab[y0:y1, x0:x1] == i
        ring = ndimage.binary_dilation(own, iterations=2) & ~own & ~cand[y0:y1, x0:x1]
        keep[i] = ring.any() and float(lum[y0:y1, x0:x1][ring].mean()) < MAX_RING
    m = keep[lab] * np.clip((mx - 0.70) / 0.25, 0.0, 1.0)
    m = ndimage.gaussian_filter(m, 0.5)
    return np.clip(m / max(float(m.max()), 1e-6), 0.0, 1.0) if m.max() > 0 else m


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", type=Path, default=ROOT / "dump_v2")
    ap.add_argument("--hd", type=Path, default=ROOT / "hd_v2")
    ap.add_argument("--sheet", type=Path, help="also write a review sheet: texture | mask")
    args = ap.parse_args()

    try:
        from terrain_hd import terrain_dumps
        seabed = set(terrain_dumps(args.dump))
    except Exception as e:                   # no original seabed set to compare with
        print("seabed atlases not recognised:", e)
        seabed = set()

    by_data: dict[str, list[Path]] = {}
    for p in sorted((args.dump / "tex").glob("*.tga")):
        # Ship and station atlases lie on black; a seabed atlas is ground
        # wall to wall (this also catches the sets terrain_hd does not know).
        a = np.asarray(Image.open(p).convert("RGB"))
        if p.stem not in seabed and (a.max(-1) < 16).mean() > 0.02:
            by_data.setdefault(p.stem.split("_")[0], []).append(p)

    out = args.hd / "emit"
    out.mkdir(parents=True, exist_ok=True)
    sheet = []
    for data, paths in by_data.items():
        ims = [np.asarray(Image.open(p).convert("RGBA")) for p in paths]
        m = np.max([mask(a) for a in ims], axis=0)
        if (m > 0.3).sum() < 4:
            continue
        Image.fromarray((m * 255).astype(np.uint8), "L").save(out / f"{data}.png")
        print(f"{data}: {(m > 0.3).sum()} texels")
        if args.sheet:
            rgb = ims[0][..., :3].astype(np.float32)
            over = rgb * 0.35 + np.array([255, 60, 40], np.float32) * m[..., None] * 0.65
            sheet.append(np.concatenate([rgb, over], 1).astype(np.uint8))
    if args.sheet and sheet:
        cols = 3
        rows = [np.concatenate(sheet[i:i + cols] + [np.zeros_like(sheet[0])] * (cols - len(sheet[i:i + cols])), 1)
                for i in range(0, len(sheet), cols)]
        Image.fromarray(np.concatenate(rows, 0)).save(args.sheet)


if __name__ == "__main__":
    main()
