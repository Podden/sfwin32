"""Upscale the dumped textures and 2D art into an HD replacement tree.

    uv run --with numpy --with pillow python tools/upscale.py [--dump dump_v2] [--hd hd]

Reads <dump>/tex/*.tga and <dump>/2d/*.tga (written by fightport --dump),
runs Real-ESRGAN (ncnn/Vulkan, local GPU) over them in one batch per kind and
writes <hd>/<kind>/<same name>.png, which fightport --hd loads in place of the
original. Existing outputs are kept, so reruns only process new dumps.

Terrain atlases are four self-repeating 128x128 tiles, which the game lays
down one per terrain quad. Upscaled as one image the network would blend
each tile's edge into its neighbour's; they are upscaled one tile at a time,
padded with their own wrapped-around edges, so every tile stays seamless.

Chroma-keyed texels are transparent in the dump. Their colour is filled from
the opaque neighbours before upscaling, so the network does not smear the key
colour into the edges, and the alpha is scaled separately and thresholded back
to the hard 0/255 cut-out the game's alpha test expects.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ESRGAN = ROOT / "third_party" / "realesrgan" / "realesrgan-ncnn-vulkan.exe"
SCALE = 4
PAD = 16            # wrap padding around a terrain tile, in source texels
MIN_SIDE = 32       # Real-ESRGAN (ncnn) hangs on very thin images: pad up to this


def bleed(rgba: np.ndarray, passes: int = 8) -> np.ndarray:
    """Fill transparent pixels' colour from opaque neighbours (edge padding)."""
    rgb = rgba[..., :3].astype(np.float32)
    known = rgba[..., 3] > 0
    for _ in range(passes):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
            cnt += k
        grow = ~known & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        known = known | grow
    return rgb.clip(0, 255).astype(np.uint8)


def is_tileset(a: np.ndarray) -> bool:
    """Opaque 256 atlas whose quadrants each wrap around smoothly."""
    if a.shape[:2] != (256, 256) or (a[..., 3] < 255).any():
        return False
    for q in range(4):
        t = a[(q >> 1) * 128:(q >> 1) * 128 + 128,
              (q & 1) * 128:(q & 1) * 128 + 128, :3].astype(np.float32)
        inner = np.abs(np.diff(t, axis=1)).mean() + 1e-6
        if max(np.abs(t[:, 0] - t[:, -1]).mean(),
               np.abs(t[0] - t[-1]).mean()) / inner > 2.5:
            return False
    return True


def run(kind: str, dump: Path, hd: Path, model: str, force: bool) -> int:
    src_dir, out_dir = dump / kind, hd / kind
    if not src_dir.is_dir():
        return 0
    out_dir.mkdir(parents=True, exist_ok=True)
    todo = [p for p in sorted(src_dir.glob("*.tga"))
            if force or not (out_dir / (p.stem + ".png")).exists()]
    if not todo:
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        tin, tout = Path(tmp) / "in", Path(tmp) / "out"
        tin.mkdir()
        tout.mkdir()
        alphas, tiled = {}, set()
        for p in todo:
            a = np.asarray(Image.open(p).convert("RGBA"))
            if kind == "tex" and is_tileset(a):
                tiled.add(p.stem)
                for q in range(4):
                    t = a[(q >> 1) * 128:(q >> 1) * 128 + 128,
                          (q & 1) * 128:(q & 1) * 128 + 128, :3]
                    Image.fromarray(np.pad(t, ((PAD, PAD), (PAD, PAD), (0, 0)),
                                           mode="wrap")).save(tin / f"{p.stem}.q{q}.png")
                continue
            if (a[..., 3] < 255).any():
                alphas[p.stem] = a[..., 3]
            rgb = bleed(a)
            py, px = max(0, MIN_SIDE - rgb.shape[0]), max(0, MIN_SIDE - rgb.shape[1])
            rgb = np.pad(rgb, ((0, py), (0, px), (0, 0)), mode="edge")
            Image.fromarray(rgb).save(tin / (p.stem + ".png"))
        subprocess.run([str(ESRGAN), "-i", str(tin), "-o", str(tout), "-n", model,
                        "-s", str(SCALE), "-f", "png"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for p in todo:
            if p.stem in tiled:
                n, c = 128 * SCALE, PAD * SCALE
                up = Image.new("RGB", (2 * n, 2 * n))
                for q in range(4):
                    t = Image.open(tout / f"{p.stem}.q{q}.png").convert("RGB")
                    up.paste(t.crop((c, c, c + n, c + n)), ((q & 1) * n, (q >> 1) * n))
                up.save(out_dir / (p.stem + ".png"))
                continue
            w, h = Image.open(p).size
            up = Image.open(tout / (p.stem + ".png")).convert("RGB")
            up = up.crop((0, 0, w * SCALE, h * SCALE))
            if p.stem in alphas:
                al = Image.fromarray(alphas[p.stem]).resize(up.size, Image.LANCZOS)
                al = al.point(lambda v: 255 if v >= 128 else 0)
                up = up.convert("RGBA")
                up.putalpha(al)
            up.save(out_dir / (p.stem + ".png"), optimize=False)
    return len(todo)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default="dump_v2")
    ap.add_argument("--hd", default="hd")
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--kinds", default="tex,2d")
    ap.add_argument("--force", action="store_true", help="redo existing outputs")
    args = ap.parse_args()
    for kind in args.kinds.split(","):
        n = run(kind, ROOT / args.dump, ROOT / args.hd, args.model, args.force)
        print(f"{kind}: {n} upscaled")


if __name__ == "__main__":
    main()
