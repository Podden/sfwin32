"""Widen the 4:3 cockpit art to 16:9 by AI outpainting (Gemini image model).

    uv run --with numpy --with pillow python tools/outpaint_cockpit.py [--dump dump_v2] [--hd hd]

For every full-screen cockpit image in <dump>/2d (640x480) this writes
<hd>/wide/<name>.png: the 16:9 picture whose centre is exactly the (upscaled)
4:3 art from <hd>/2d/<name>.png and whose sides are painted by the model as
more cockpit -- solid panels, no window, since the 3D view stays 4:3 and
nothing would be rendered behind a widened window. fightport draws those
sides beside the 4:3 frame while that cockpit is on screen.

Each cockpit also exists as a half-bright copy (the game dims the art); its
sides are the bright sides scaled by the measured per-channel ratio, so both
match without a second, different painting. Every API call is appended to
runs/api_spend.csv.
"""
import argparse
import base64
import csv
import io
import json
import time
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SECRET = Path(r"C:\Users\daniel.pots\.claude\secrets\gemini_api_key")
H = 1920                       # output height = 4x upscaled art
W43, W169 = H * 4 // 3, H * 16 // 9
BAND = 64                      # seam blend, pixels inside the 4:3 edge
USD_PER_IMAGE = 0.24           # gemini-3-pro-image at 4K, list price

PROMPT = (
    "Image 1 is the cockpit view of a 1990s deep-sea combat submarine (pre-rendered CG, the game "
    "Archimedean Dynasty), centred on a wider 16:9 canvas. The flat grey bands left and right are "
    "empty. Extend the cockpit into them: continue the hull frame, struts, pipes, cables, rivets and "
    "instrument housings outward so the cockpit reads as wider. The extensions are SOLID interior -- "
    "no windows, no openings, no view outside, no black areas. Keep the existing centre exactly as it "
    "is, same perspective, same dim lighting, same colours and materials, same level of detail. "
    "No text, no screens with writing, no people, no watermark, no border."
)


def generate(key: str, image: Image.Image, prompt: str, model: str) -> Image.Image:
    buf = io.BytesIO()
    image.save(buf, "PNG")
    body = {
        "model": model,
        "input": [{"type": "text", "text": prompt},
                  {"type": "image", "mime_type": "image/png",
                   "data": base64.b64encode(buf.getvalue()).decode()}],
        "response_format": {"type": "image", "aspect_ratio": "16:9", "image_size": "4K"},
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
    f.parent.mkdir(exist_ok=True)
    with f.open("a", newline="") as fh:
        csv.writer(fh).writerow([time.strftime("%Y-%m-%d %H:%M"), "gemini", what, USD_PER_IMAGE])


def compose(painted: np.ndarray) -> np.ndarray:
    """RGBA, W169 x H: the painted sides opaque, fading out over BAND pixels
    into the 4:3 frame, which fightport draws underneath (the live game
    picture, so the original art there is never altered)."""
    x0 = (W169 - W43) // 2
    alpha = np.full(W169, 255.0, np.float32)
    alpha[x0:x0 + W43] = 0
    alpha[x0:x0 + BAND] = np.linspace(255, 0, BAND)
    alpha[x0 + W43 - BAND:x0 + W43] = np.linspace(0, 255, BAND)
    a = np.broadcast_to(alpha[None, :, None], (H, W169, 1))
    return np.concatenate([painted.clip(0, 255), a], axis=2).astype(np.uint8)


def pairs(dump: Path) -> list[tuple[Path, Path | None, np.ndarray | None]]:
    """(bright, dim or None, per-channel dim/bright ratio) per cockpit."""
    fs = sorted((dump / "2d").glob("*_640x480.tga"))
    ims = {f: np.asarray(Image.open(f).convert("RGB")).astype(np.float32) for f in fs}
    ims = {f: a for f, a in ims.items() if a.mean() > 2}          # skip the black frame
    used, out = set(), []
    for b in sorted(ims, key=lambda f: -ims[f].mean()):
        if b in used:
            continue
        used.add(b)
        best = None
        for d in ims:
            if d in used:
                continue
            m = ims[b].sum(2) > 60
            r = ims[d][m] / (ims[b][m] + 1e-3)
            if 0.2 < np.median(r) < 0.95 and np.std(np.clip(r, 0, 2)) < 0.25:
                best = (d, np.median(r, axis=0))
                break
        if best:
            used.add(best[0])
            out.append((b, best[0], best[1]))
        else:
            out.append((b, None, None))
    return out


def hd_art(hd: Path, tga: Path) -> np.ndarray:
    """The 4:3 art at output size: the upscaled version if there is one."""
    p = hd / "2d" / (tga.stem + ".png")
    im = Image.open(p) if p.exists() else Image.open(tga)
    return np.asarray(im.convert("RGB").resize((W43, H), Image.LANCZOS)).astype(np.float32)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default="dump_v2")
    ap.add_argument("--hd", default="hd")
    ap.add_argument("--model", default="gemini-3-pro-image")
    ap.add_argument("--only", default="", help="comma-separated bright cockpit names")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    dump, hd = ROOT / args.dump, ROOT / args.hd
    (hd / "wide").mkdir(parents=True, exist_ok=True)
    key = SECRET.read_text(encoding="utf-8").strip()

    for bright, dim, ratio in pairs(dump):
        if args.only and bright.stem not in args.only.split(","):
            continue
        dst = hd / "wide" / (bright.stem + ".png")
        raw = ROOT / "runs" / "outpaint" / (bright.stem + ".png")   # the model's own output
        raw.parent.mkdir(parents=True, exist_ok=True)
        if not raw.exists() or args.force:
            center = hd_art(hd, bright)
            canvas = Image.new("RGB", (W169, H), (128, 128, 128))
            canvas.paste(Image.fromarray(center.astype(np.uint8)), ((W169 - W43) // 2, 0))
            painted = generate(key, canvas.resize((W169 // 2, H // 2), Image.LANCZOS),
                               PROMPT, args.model)
            log_spend(f"outpaint {bright.stem}")
            painted.save(raw)
            print("painted", bright.stem)
        painted = np.asarray(Image.open(raw).convert("RGB").resize((W169, H), Image.LANCZOS),
                             dtype=np.float32)
        Image.fromarray(compose(painted)).save(dst)
        if dim is not None:
            Image.fromarray(compose(painted * ratio[None, None, :])).save(
                hd / "wide" / (dim.stem + ".png"))
            print("dimmed ", dim.stem, ratio.round(3))


if __name__ == "__main__":
    main()
