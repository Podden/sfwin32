"""Top-down preview of a terrain as the game lays it out, to judge seams offline.

    uv run --with numpy --with pillow python tools/terrain_sim.py [--surface ADAM] [--tiles DIR] [--out PNG]

Cells come from SURFACE.MAP (type*4 + quadrant) and SURFACE.DIR (orientation,
through DIR_REMAP), as documented in Schleichfahrt2/docs/formats/SURFACE.md.
--tiles is a directory of S000_NNN.png atlases (any size, 2x2 quadrants);
default is the converted originals.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "game" / "SF" / "VFX" / "SURFACE"
ORIG = Path(r"D:\Projekte\Schleichfahrt2\extracted\converted\VFX\TEXTURE")
TEX_MAPPING = [0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 10, 11, 12, 13, 14, 9, 16]
DIR_REMAP = [1, 3, 7, 5, 6, 4, 0, 2]


def load_grid(name: str, which: str) -> np.ndarray:
    raw = (GAME / name / f"SURFACE.{which}").read_bytes()
    return np.frombuffer(raw[11:11 + 65536], np.uint8).reshape(256, 256)


def quadrants(path: Path, n: int) -> list[np.ndarray]:
    a = np.asarray(Image.open(path).convert("RGB").resize((2 * n, 2 * n), Image.LANCZOS))
    return [a[(q >> 1) * n:(q >> 1) * n + n, (q & 1) * n:(q & 1) * n + n] for q in range(4)]


def orient(t: np.ndarray, o: int) -> np.ndarray:
    t = np.rot90(t, o // 2)
    return t[:, ::-1] if o % 2 else t


def compose(surface: str, tiles: Path, x0: int, y0: int, cells: int, n: int) -> np.ndarray:
    m, d = load_grid(surface, "MAP"), load_grid(surface, "DIR")
    cache: dict[int, list[np.ndarray]] = {}
    out = np.zeros((cells * n, cells * n, 3), np.uint8)
    for cy in range(cells):
        for cx in range(cells):
            v, o = int(m[y0 + cy, x0 + cx]), DIR_REMAP[d[y0 + cy, x0 + cx]]
            f = TEX_MAPPING[v // 4]
            if f not in cache:
                cache[f] = quadrants(tiles / f"S000_{f:03d}.png", n)
            out[cy * n:cy * n + n, cx * n:cx * n + n] = orient(cache[f][v % 4], o)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--surface", default="ADAM")
    ap.add_argument("--tiles", type=Path, default=ORIG)
    ap.add_argument("--at", default="100,100")
    ap.add_argument("--cells", type=int, default=8)
    ap.add_argument("--px", type=int, default=128, help="pixels per cell")
    ap.add_argument("--out", type=Path, default=ROOT / "runs" / "terrain" / "sim.png")
    a = ap.parse_args()
    x0, y0 = (int(v) for v in a.at.split(","))
    Image.fromarray(compose(a.surface, a.tiles, x0, y0, a.cells, a.px)).save(a.out)
    print(a.out)


if __name__ == "__main__":
    main()
