"""New HD seabed: AI-repainted terrain atlases with relief, seamless in any orientation.

    uv run --with numpy --with scipy --with pillow python tools/terrain_hd.py gen [--only STEM,..]
    uv run --with numpy --with scipy --with pillow python tools/terrain_hd.py build [--hd hd_v2]

The seabed is 13 used 256x256 atlases (S000_NNN), each four 128x128 tiles
the game lays down one per terrain cell in any of 8 rotations/mirrors.

gen    Gemini repaints every atlas found in <dump>/tex at 1K (= N per tile), keeping the
       layout (veins, patches, coral/rock transitions), adding photographic
       detail. Raw results are kept in runs/terrain/gemini/; every call is
       logged to runs/api_spend.csv.
build  per tile, at N=512:
       - the original's large-scale colour is kept (low-pass of the
         original, high-pass of the painting), so the HD seabed has the
         original's colours and material layout exactly;
       - DeepBump derives a normal map; its z component and a cavity term
         from the derived height are multiplied in. Light from straight
         above only: the tiles are rotated per cell, any other direction
         would light neighbouring cells from different sides;
       - tools/seamless.harmonize makes every edge fit every other.
       Writes <hd>/tex/<stem>.png (the 1024 atlas fightport loads in place
       of the original) and the normal/height maps next to the raw output.
"""
import argparse
import base64
import csv
import io
import json
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from seamless import harmonize  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SECRET = Path(r"C:\Users\daniel.pots\.claude\secrets\gemini_api_key")
ORIG = Path(r"D:\Projekte\Schleichfahrt2\extracted\converted\VFX\TEXTURE")
DEEPBUMP = Path(r"D:\Projekte\Schleichfahrt2\research\DeepBump")
RAW = ROOT / "runs" / "terrain" / "gemini"
N = 512                      # output texels per tile (4x the original)
USD_PER_IMAGE = 0.067        # gemini-3.1-flash-image (Nano Banana 2) at 1K, list price

PROMPT = (
    "Image 1 is a texture atlas from a 1996 underwater game: four square seabed tiles in a 2x2 grid "
    "(each quarter of the image is one separate tile). Repaint it as a high-resolution photographic "
    "texture of the same deep-sea floor. Keep the layout of every tile exactly: every pale vein, crack, "
    "ridge, dark patch, coral or sand area at the same position, size and shape, with the same colours "
    "and brightness. Add realistic fine detail at every scale: rock grain, mineral crust, silt in the "
    "cracks, small pebbles and fragments, tiny pores. Orthographic top-down view. Flat, even, diffuse "
    "light from directly above: no cast shadows, no highlights, no light rays, no caustics, no water "
    "haze, no fish, no objects, no text, no watermark, no border, no lines between the four tiles."
)


def terrain_dumps(dump: Path) -> dict[str, int]:
    """dump stem -> S000 index, for every dumped 256 texture that is a terrain atlas."""
    def z(a: np.ndarray) -> np.ndarray:
        a = a - a.mean()
        return a / (a.std() + 1e-6)
    ref = {i: z(np.asarray(Image.open(ORIG / f"S000_{i:03d}.png").convert("L").resize((64, 64)),
                           np.float32)) for i in range(17)}
    out = {}
    for p in sorted((dump / "tex").glob("*.tga")):
        im = Image.open(p)
        if im.size != (256, 256):
            continue
        d = z(np.asarray(im.convert("L").resize((64, 64)), np.float32))
        score, i = max((float((r * d).mean()), i) for i, r in ref.items())
        if score > 0.95:
            out[p.stem] = i
    return out


def generate(key: str, image: Image.Image, model: str) -> Image.Image:
    buf = io.BytesIO()
    image.save(buf, "PNG")
    body = {
        "model": model,
        "input": [{"type": "text", "text": PROMPT},
                  {"type": "image", "mime_type": "image/png",
                   "data": base64.b64encode(buf.getvalue()).decode()}],
        "response_format": {"type": "image", "aspect_ratio": "1:1", "image_size": "1K"},
    }
    req = urllib.request.Request(
        "https://generativelanguage.googleapis.com/v1beta/interactions",
        data=json.dumps(body).encode(),
        headers={"x-goog-api-key": key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        out = json.load(r)
    data = out.get("output_image", {}).get("data")
    for step in out.get("steps", []):
        for block in step.get("content", []):
            if not data and block.get("type") == "image":
                data = block["data"]
    if not data:
        raise RuntimeError("no image in response: " + json.dumps(out)[:600])
    return Image.open(io.BytesIO(base64.b64decode(data))).convert("RGB")


def log_spend(what: str) -> None:
    f = ROOT / "runs" / "api_spend.csv"
    with f.open("a", newline="") as fh:
        csv.writer(fh).writerow([time.strftime("%Y-%m-%d %H:%M"), "gemini", what, USD_PER_IMAGE])


def deepbump(src: Path, dst: Path, mode: str, *extra: str) -> None:
    subprocess.run(["uv", "run", "-q", "--with", "onnxruntime", "--with", "imageio",
                    "--with", "numpy", "python", "cli.py", str(src), str(dst), mode, *extra],
                   cwd=DEEPBUMP, check=True, capture_output=True,
                   env={**__import__("os").environ, "PYTHONIOENCODING": "utf-8"})


def quads(a: np.ndarray) -> list[np.ndarray]:
    n = a.shape[0] // 2
    return [a[(q >> 1) * n:(q >> 1) * n + n, (q & 1) * n:(q & 1) * n + n] for q in range(4)]


def relief(rgb: np.ndarray, work: Path, name: str) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """(shading factor, normal map, height) for one tile, from DeepBump.
    Worked on a mirror-padded tile: DeepBump's normals and height drift at
    an image border, which would come back as a dark line along every cell
    edge."""
    src, nrm, hgt = work / f"{name}.png", work / f"{name}_n.png", work / f"{name}_h.png"
    p = N // 16
    Image.fromarray(np.pad(rgb, ((p, p), (p, p), (0, 0)), mode="reflect")
                    .clip(0, 255).astype(np.uint8)).save(src)
    deepbump(src, nrm, "color_to_normals", "--color_to_normals-overlap", "LARGE")
    deepbump(nrm, hgt, "normals_to_height", "--normals_to_height-seamless", "FALSE")
    n = np.asarray(Image.open(nrm).convert("RGB"), np.float32) / 127.5 - 1.0
    h = np.asarray(Image.open(hgt).convert("L"), np.float32) / 255.0
    nz = np.clip(n[..., 2], 0, 1)
    # cavity: how far a texel lies below its surroundings, at two scales
    cav = sum(np.clip(ndimage.gaussian_filter(h, s, mode="reflect") - h, 0, None) for s in (4, 16))
    crop = np.s_[p:-p, p:-p]
    n, h, nz, cav = n[crop], h[crop], nz[crop], cav[crop]
    shade = (0.55 + 0.45 * nz) * np.clip(1.0 - 2.5 * cav, 0.35, 1.0)
    return shade / shade.mean(), n, h


def build(stem: str, dump: Path, hd: Path, lf_sigma: float, edge_sigma: float,
          detail: float) -> None:
    orig = np.asarray(Image.open(dump / "tex" / f"{stem}.tga").convert("RGB")
                      .resize((2 * N, 2 * N), Image.BICUBIC), np.float32)
    gen = np.asarray(Image.open(RAW / f"{stem}.png").convert("RGB")
                     .resize((2 * N, 2 * N), Image.LANCZOS), np.float32)
    lf = lambda a: ndimage.gaussian_filter(a, (lf_sigma, lf_sigma, 0), mode="reflect")
    out = np.zeros_like(gen)
    with tempfile.TemporaryDirectory() as tmp:
        for q, (o, g) in enumerate(zip(quads(orig), quads(gen))):
            t = lf(o) + detail * (g - lf(g))
            shade, n, h = relief(t, Path(tmp), f"q{q}")
            t = harmonize(t * shade[..., None], edge_sigma)
            n0, n1 = (q >> 1) * N, (q & 1) * N
            out[n0:n0 + N, n1:n1 + N] = t
            Image.fromarray(((n + 1) * 127.5).clip(0, 255).astype(np.uint8)).save(
                RAW / f"{stem}.q{q}_normal.png")
            Image.fromarray((h * 255).clip(0, 255).astype(np.uint8)).save(RAW / f"{stem}.q{q}_height.png")
    (hd / "tex").mkdir(parents=True, exist_ok=True)
    Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(hd / "tex" / f"{stem}.png")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("step", choices=["gen", "build"])
    ap.add_argument("--dump", default="dump_v2")
    ap.add_argument("--hd", default="hd_v2")
    ap.add_argument("--only", default="")
    ap.add_argument("--model", default="gemini-3.1-flash-image")
    ap.add_argument("--lf", type=float, default=N / 64, help="sigma of the original's colour kept")
    ap.add_argument("--edge", type=float, default=6 * N / 128, help="harmonize() sigma")
    ap.add_argument("--detail", type=float, default=0.6, help="contrast of the painted detail")
    a = ap.parse_args()
    dump, hd = ROOT / a.dump, ROOT / a.hd
    stems = terrain_dumps(dump)
    if a.only:
        stems = {s: i for s, i in stems.items() if any(s.startswith(o) for o in a.only.split(","))}
    RAW.mkdir(parents=True, exist_ok=True)
    if a.step == "gen":
        key = SECRET.read_text(encoding="utf-8").strip()
        for stem, i in stems.items():
            if (RAW / f"{stem}.png").exists():
                continue
            ref = Image.open(dump / "tex" / f"{stem}.tga").convert("RGB").resize((2 * N, 2 * N), Image.LANCZOS)
            img = generate(key, ref, a.model)
            log_spend(f"terrain S000_{i:03d} {stem}")
            img.save(RAW / f"{stem}.png")
            print("painted", stem, f"S000_{i:03d}", img.size, flush=True)
    else:
        for stem in stems:
            if (RAW / f"{stem}.png").exists():
                build(stem, dump, hd, a.lf, a.edge, a.detail)
                print("built", stem, flush=True)


if __name__ == "__main__":
    main()
