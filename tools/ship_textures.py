"""Resolution tiers for ship/station textures: big hulls get more texels than small craft.

    uv run --with numpy --with pillow python tools/ship_textures.py map [--dump dump_v2]
    uv run --with numpy --with pillow python tools/ship_textures.py up  [--dump dump_v2] [--hd hd_v2]

map  Reads every original model (VFX/MODULE/*.gltf), applies the node transforms and
     takes the largest axis-aligned extent of each model (game units). Every
     MOD_* texture inherits the largest extent among the models that use it.
     Each dumped texture is matched to its original by correlating 64x64
     greyscale (>0.95; the dumps are the originals at 256x256, chroma-keyed
     texels are transparent). Writes runs/ships/texture_sizes.csv:
     dumpstem, original, models, max_extent, tier, scale.
up   For every dump of a scale>4 tier: copies the existing 4x file to
     runs/ships/backup_4x/ (once), then writes scale x original through
     Real-ESRGAN (via tools/upscale.py: bleed, hard 0/255 alpha). 8x is the
     4x model twice (16x), Lanczos down to 8x. Terrain atlases (S000/S001)
     are not matched here and are never touched.

Model size. The MOD format stores int16 coordinates, so everything up to ~260 units
(scouts 130-300, Moogh boat 247, container freighter 271) is in raw units. Hulls too
big for that (SEACORE0-7, POLAR0-7, TRITON, IFRACHT, CONTFRTA-G, KUPPEL, ELTOPOA-D,
RWOHMO, BFM, BIONTOD, ABGEB010+) are stored as unit-sized floats (extent 1-2.6) and
scaled up by the engine, so their raw extent says nothing. A model with extent < 5
is therefore "normalised" and counts as 1000; 5-20 are guns/arm/body parts (small).
Tiers (extent = largest of the models using the texture):
  small  < 400    scouts, Moogh boat, bombers, torpedoes, mines, turrets, parts  4x  (1024 px)
  large  >= 400   Seacore, Polar, Triton, freighters, domes, big bases           8x  (2048 px)
A texture shared by a small and a large model counts as large: the big hull
is where the texels get stretched. 2048 is the cap (GL_MAX_TEXTURE_SIZE >= 4096
everywhere that matters, and fightport takes any size; the 256 atlas is
cut into four 1024 tiles). Thresholds: see the extent column in the CSV.
"""
import argparse
import csv
import json
import math
import subprocess
import shutil
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from upscale import ESRGAN, MIN_SIDE, bleed  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
VFX = Path(r"D:\Projekte\Schleichfahrt2\extracted\converted\VFX")
CSV = ROOT / "runs" / "ships" / "texture_sizes.csv"
BACKUP = ROOT / "runs" / "ships" / "backup_4x"
LARGE = 400.0


def mat(n: dict) -> np.ndarray:
    if "matrix" in n:
        return np.array(n["matrix"], float).reshape(4, 4).T
    x, y, z, w = n.get("rotation", [0, 0, 0, 1])
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * np.array(n.get("scale", [1, 1, 1]))
    m[:3, 3] = n.get("translation", [0, 0, 0])
    return m


def model_info(path: Path):
    """(max extent, set of texture stems) of one glTF, node transforms applied."""
    g = json.loads(path.read_text())
    lo, hi = np.full(3, math.inf), np.full(3, -math.inf)
    nodes = g.get("nodes", [])

    def walk(i, parent):
        nonlocal lo, hi
        n = nodes[i]
        m = parent @ mat(n)
        if "mesh" in n:
            for p in g["meshes"][n["mesh"]]["primitives"]:
                a = g["accessors"][p["attributes"]["POSITION"]]
                c = np.array([[x, y, z, 1] for x in (a["min"][0], a["max"][0])
                              for y in (a["min"][1], a["max"][1])
                              for z in (a["min"][2], a["max"][2])]) @ m.T
                lo, hi = np.minimum(lo, c[:, :3].min(0)), np.maximum(hi, c[:, :3].max(0))
        for c in n.get("children", []):
            walk(c, m)

    for s in g.get("scenes", [{"nodes": range(len(nodes))}])[:1]:
        for i in s["nodes"]:
            walk(i, np.eye(4))
    tex = {Path(i["uri"]).stem.upper() for i in g.get("images", [])}
    return float((hi - lo).max()), tex


def z64(im: Image.Image) -> np.ndarray:
    a = np.asarray(im.convert("L").resize((64, 64)), np.float32)
    return (a - a.mean()) / (a.std() + 1e-6)


def build_map(dump: Path) -> None:
    ext, users = {}, defaultdict(list)
    for p in sorted((VFX / "MODULE").glob("*.gltf")):
        if p.stem == "SQUARE":      # unit quad (effect plane), not a hull
            continue
        e, tex = model_info(p)
        ext[p.stem] = 1000.0 if e < 5 else e
        for t in tex:
            users[t].append(p.stem)
    ref = {p.stem: z64(Image.open(p)) for p in sorted((VFX / "TEXTURE").glob("MOD_*.png"))}
    rows = []
    for p in sorted((dump / "tex").glob("*.tga")):
        d = z64(Image.open(p))
        score, name = max((float((r * d).mean()), n) for n, r in ref.items())
        if score < 0.95:
            rows.append([p.stem, "", "", "", "", "", ""])
            continue
        m = sorted(users.get(name, []), key=lambda k: -ext[k])
        mx = max((ext[k] for k in m), default=0.0)
        big = mx >= LARGE
        norm = any(k for k in m if ext[k] == 1000.0)
        rows.append([p.stem, name, " ".join(m), f"{mx:.0f}", "large" if big else "small",
                     8 if big else 4, "normalised" if norm else "raw"])
    CSV.parent.mkdir(parents=True, exist_ok=True)
    with open(CSV, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["dumpstem", "original", "models", "max_extent", "tier", "scale", "basis"])
        w.writerows(rows)
    t = defaultdict(int)
    for r in rows:
        t[r[4] or "terrain/unmatched"] += 1
    print(dict(t), "->", CSV)


def esrgan(src: Path, dst: Path, model: str, scale: int) -> None:
    subprocess.run([str(ESRGAN), "-i", str(src), "-o", str(dst), "-n", model, "-s",
                    str(scale), "-f", "png"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def upscale(dump: Path, hd: Path, model: str) -> None:
    rows = [r for r in csv.DictReader(open(CSV)) if r["scale"] and int(r["scale"]) > 4]
    BACKUP.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        (t / "in").mkdir(), (t / "o1").mkdir(), (t / "o2").mkdir()
        alphas = {}
        for r in rows:
            s = r["dumpstem"]
            a = np.asarray(Image.open(dump / "tex" / f"{s}.tga").convert("RGBA"))
            if (a[..., 3] < 255).any():
                alphas[s] = a[..., 3]
            Image.fromarray(bleed(a)).save(t / "in" / f"{s}.png")
            old = hd / "tex" / f"{s}.png"
            if old.exists() and not (BACKUP / old.name).exists():
                shutil.copy2(old, BACKUP / old.name)
        # 8x = the 4x model twice, then down to 8x. Not -s 2 on the second
        # pass: the x4 models only do 4x, and ncnn then stitches its tiles
        # back in the wrong places.
        esrgan(t / "in", t / "o1", model, 4)
        esrgan(t / "o1", t / "o2", model, 4)
        for r in rows:
            s = r["dumpstem"]
            up = Image.open(t / "o2" / f"{s}.png").convert("RGB")
            up = up.resize((up.width // 2, up.height // 2), Image.LANCZOS)
            if s in alphas:
                al = Image.fromarray(alphas[s]).resize(up.size, Image.LANCZOS)
                up = up.convert("RGBA")
                up.putalpha(al.point(lambda v: 255 if v >= 128 else 0))
            up.save(hd / "tex" / f"{s}.png")
            print(s, r["original"], up.size)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["map", "up"])
    ap.add_argument("--dump", default="dump_v2")
    ap.add_argument("--hd", default="hd_v2")
    ap.add_argument("--model", default="realesrgan-x4plus")
    a = ap.parse_args()
    if a.cmd == "map":
        build_map(ROOT / a.dump)
    else:
        upscale(ROOT / a.dump, ROOT / a.hd, a.model)


if __name__ == "__main__":
    main()
