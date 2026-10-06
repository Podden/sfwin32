"""New 16:9 cockpits with a much larger window, for fightport's Hor+ mode.

    uv run --with numpy --with scipy --with pillow python tools/cockpit_wide.py [--only NAME,..] [--force]

Under Hor+ the game renders 3D across the whole 16:9 frame, so a cockpit no
longer has to frame a 4:3 view. Per bright cockpit image in <dump>/2d:

1. canvas  16:9, the 4:3 HD art in the middle. What has to stay exactly where
           it is -- the console with the target monitor and both radars, the
           top weapon display -- is kept; everything else of the old frame is
           grey ("paint cockpit here") and the new, far bigger window is flat
           magenta.
2. paint   Gemini (Nano Banana Pro, 4K) paints a cockpit frame around that
           window; raw output in runs/cockpit/.
3. build   alpha from the magenta (soft-edged), the kept original pixels pasted
           back exactly (feathered), the dimmed twin by the measured ratio.
           Writes <hd>/cockpit/<name>.png at 4x (3413x1920, RGBA, window
           transparent), which fightport draws over the full width and uses
           as the cockpit mask.
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from outpaint_cockpit import SECRET, generate, hd_art, log_spend, pairs  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
S = 4                                  # output scale over 640x480
H, W43 = 480 * S, 640 * S
W = H * 16 // 9                        # 3413
X0 = (W - W43) // 2                    # where the 4:3 art sits
MAGENTA = np.array([255, 0, 255], np.float32)
GREY = np.array([128, 128, 128], np.float32)

PROMPT = (
    "Image 1 is a layout for the cockpit view of a deep-sea combat submarine in a 1990s CG game "
    "(Archimedean Dynasty), 16:9. Three kinds of area: the detailed instrument parts (console with "
    "monitor and round radar instruments at the bottom, a small display at the top) are final; the "
    "flat MAGENTA area is the cockpit's big panoramic window; the flat GREY areas are where you paint "
    "the cockpit structure. Paint the cockpit frame in the grey areas only: a slim heavy-metal hull "
    "frame along the screen edges, the console continued to both sides with panels, pipes, cables and "
    "rivets, thin struts, in the same materials, colours, dim lighting and level of detail as the "
    "instrument parts, so it all reads as one cockpit seen from the pilot's seat. Paint over every "
    "grey pixel, up to the screen edges and right up to the instrument parts: no flat grey may "
    "remain anywhere. Keep the instrument "
    "parts unchanged and in place. Keep every magenta pixel exactly flat pure magenta #FF00FF -- do "
    "not draw anything into the window, no glass, no reflections, no view outside. No text, no "
    "people, no watermark, no border."
)


# Where each cockpit's instruments are (x0, y0, x1, y1 in 640x480): the game
# draws live content into them (monitors, radars, weapon display), so they
# must stay exactly in place. Read off the four cockpits by eye.
INSTRUMENTS = {
    # green: the old frame's struts reach into the console rect beside both
    # radars and would end there as stubs, so the sides start lower
    "d419d4a2": [(110, 290, 530, 480), (60, 348, 110, 480), (530, 348, 580, 480),
                 (245, 0, 395, 104)],
    "a216526a": [(0, 0, 150, 112), (138, 58, 242, 172), (398, 58, 502, 172),
                 (488, 0, 640, 112), (192, 298, 444, 480)],              # red
    "ba8f8010": [(0, 0, 112, 112), (528, 0, 640, 112), (268, 0, 402, 56),
                 (74, 288, 566, 480)],                                    # blue
    "d850424e": [(0, 0, 132, 108), (508, 0, 640, 108), (118, 298, 522, 480)],  # yellow
}


def layout(o: np.ndarray, rects) -> tuple[np.ndarray, np.ndarray]:
    """(keep, window) masks on the 16:9 canvas at 640-scale (853x480), from
    the original 640x480 art o. keep: original pixels that stay, window: the
    new window."""
    w169 = 480 * 16 // 9
    x0 = (w169 - 640) // 2
    art = o.sum(2) > 0                                  # old window is pure black
    keep = np.zeros((480, w169), bool)
    win = np.zeros((480, w169), bool)
    k = np.zeros_like(art)
    for rx0, ry0, rx1, ry1 in rects:
        k[ry0:ry1, rx0:rx1] = True
    k = k & art
    # only whole parts: drop thin bits of the old frame ring that reach in
    k = ndimage.binary_opening(k, iterations=3)
    keep[:, x0:x0 + 640] = k
    # the new window: almost the whole frame above the console
    yy, xx = np.mgrid[0:480, 0:w169]
    m, top, bot = 18, 26, 360                           # edge frame, top, bottom
    win = (xx > m) & (xx < w169 - m) & (yy > top) & (yy < bot)
    corner = 60                                         # rounded corners
    for cx, cy in ((m + corner, top + corner), (w169 - m - corner, top + corner),
                   (m + corner, bot - corner), (w169 - m - corner, bot - corner)):
        q = ((xx - cx) * np.sign(cx - w169 / 2) > 0) & ((yy - cy) * np.sign(cy - 240) > 0)
        win &= ~(q & ((xx - cx) ** 2 + (yy - cy) ** 2 > corner ** 2))
    win &= ~ndimage.binary_dilation(keep, iterations=10)
    # whatever of the old window lies outside that is window too
    old = np.zeros_like(win)
    old[:, x0:x0 + 640] = ~art
    win |= old & (yy < bot) & ~ndimage.binary_dilation(keep, iterations=10)
    return keep, win


def up(mask: np.ndarray) -> np.ndarray:
    """640-scale mask -> output size, smooth (float 0..1)."""
    im = Image.fromarray((mask * 255).astype(np.uint8)).resize((W, H), Image.BILINEAR)
    return np.asarray(im, np.float32) / 255.0


def canvas(hd: np.ndarray, keep: np.ndarray, win: np.ndarray) -> Image.Image:
    c = np.empty((H, W, 3), np.float32)
    c[:] = GREY
    full = np.zeros_like(c)
    full[:, X0:X0 + W43] = hd
    k, w = up(keep)[..., None], up(win)[..., None]
    c = c * (1 - k) + full * k
    c = c * (1 - w) + MAGENTA * w
    return Image.fromarray(c.clip(0, 255).astype(np.uint8))


def build(painted: np.ndarray, hd: np.ndarray, keep: np.ndarray, win: np.ndarray,
          window_min: bool = True) -> np.ndarray:
    """RGBA output: painted frame, original instruments, transparent window."""
    # magenta distance -> alpha: the model's own window shape decides (it
    # draws struts and pipes into the layout's window, and that is wanted)
    d = np.linalg.norm(painted - MAGENTA, axis=2) / 160.0
    alpha = np.clip(d - 0.35, 0, 1)
    # the model leaves the narrow grey margin around the kept instruments
    # unpainted: fill it, colour and alpha, from the nearest painted pixel
    rgb = painted.copy()
    sd = np.sqrt(np.maximum(ndimage.uniform_filter(rgb.mean(2) ** 2, 5)
                            - ndimage.uniform_filter(rgb.mean(2), 5) ** 2, 0))
    near = up(ndimage.binary_dilation(keep, iterations=16)) > 0.5
    grey = (np.abs(rgb - GREY).max(2) < 22) & (sd < 6) & near & ~(up(keep) > 0.5)
    grey = ndimage.binary_dilation(grey, iterations=2) & near & ~(up(keep) > 0.5)
    iy, ix = ndimage.distance_transform_edt(grey, return_distances=False, return_indices=True)
    rgb, alpha = rgb[iy, ix], alpha[iy, ix]
    alpha = ndimage.gaussian_filter(alpha, 1.2)
    # magenta fringe on the frame's edge: pull the colour towards the frame
    spill = np.clip(np.minimum(rgb[..., 0], rgb[..., 2]) - rgb[..., 1], 0, None)
    rgb[..., 0] -= spill * 0.8
    rgb[..., 2] -= spill * 0.8
    full = np.zeros_like(rgb)
    full[:, X0:X0 + W43] = hd
    kk = up(keep)                                            # feathered inwards
    k = (ndimage.gaussian_filter(kk, 4.0) * kk)[..., None]
    rgb = rgb * (1 - k) + full * k
    a = np.maximum(alpha, k[..., 0])
    # The model likes to close the window in again with its own frame; the
    # point of these cockpits is the view, so the layout's window is cut out
    # regardless, and the cut gets a bevelled rim so it reads as the frame's
    # edge rather than a hole.
    if window_min:
        # right up to the instruments: the margin left for the model to mount
        # them in otherwise comes back as a pale outline
        tight = win | (ndimage.binary_dilation(win, iterations=10)
                       & ~ndimage.binary_dilation(keep, iterations=1))
        a = np.minimum(a, 1.0 - np.clip(ndimage.gaussian_filter(up(tight), 1.5) * 2 - 0.5, 0, 1)
                       * (1 - k[..., 0]))
        dist = ndimage.distance_transform_edt(a > 0.5)       # px from the window
        rim = 0.35 + 0.65 * np.clip(dist / (6 * S), 0, 1) ** 0.7
        rim += 0.5 * np.exp(-((dist - 1.5 * S) / (0.8 * S)) ** 2)   # lit lip
        rgb = rgb * np.where(k[..., 0] > 0.5, 1.0, rim)[..., None]
    return np.concatenate([rgb.clip(0, 255), (a * 255)[..., None]], axis=2).astype(np.uint8)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default="dump_v2")
    ap.add_argument("--hd", default="hd_v2")
    ap.add_argument("--model", default="gemini-3-pro-image")
    ap.add_argument("--only", default="")
    ap.add_argument("--force", action="store_true", help="paint again")
    a = ap.parse_args()
    dump, hd_dir = ROOT / a.dump, ROOT / a.hd
    out_dir, raw_dir = hd_dir / "cockpit", ROOT / "runs" / "cockpit"
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_dir.mkdir(parents=True, exist_ok=True)
    key = None
    for bright, dim, ratio in pairs(dump):
        if a.only and bright.stem not in a.only.split(","):
            continue
        o = np.asarray(Image.open(bright).convert("RGB"))
        if (o.sum(2) == 0).mean() < 0.2 or (o.sum(2) == 0).mean() > 0.9:
            continue                                     # not a cockpit
        rects = INSTRUMENTS.get(bright.stem[:8])
        if rects is None:
            print("no instrument layout for", bright.stem)
            continue
        hd = hd_art(hd_dir, bright)
        keep, win = layout(o, rects)
        raw = raw_dir / f"{bright.stem}.png"
        if not raw.exists() or a.force:
            cv = canvas(hd, keep, win)
            cv.save(raw_dir / f"{bright.stem}_layout.png")
            key = key or SECRET.read_text(encoding="utf-8").strip()
            img = generate(key, cv.resize((W // 2, H // 2), Image.LANCZOS), PROMPT, a.model)
            log_spend(f"cockpit_wide {bright.stem}")
            img.save(raw)
            print("painted", bright.stem, img.size, flush=True)
        painted = np.asarray(Image.open(raw).convert("RGB").resize((W, H), Image.LANCZOS), np.float32)
        rgba = build(painted, hd, keep, win)
        Image.fromarray(rgba).save(out_dir / f"{bright.stem}.png")
        if dim is not None:
            d = rgba.astype(np.float32)
            d[..., :3] *= ratio[None, None, :]
            Image.fromarray(d.clip(0, 255).astype(np.uint8)).save(out_dir / f"{dim.stem}.png")
        print("built", bright.stem, "+ dim" if dim is not None else "", flush=True)


if __name__ == "__main__":
    main()
