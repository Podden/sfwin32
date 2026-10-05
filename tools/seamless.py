"""Make terrain tiles fit each other along every edge, in any orientation.

The game lays every terrain cell down as one of 4 x 17 tiles in any of the
8 rotations/mirrors, and the tiles were never drawn to match each other, so
every cell edge is a seam (~1.4x the texture's own texel-to-texel step).

harmonize() gives every tile edge the same high-frequency content -- none:
only the edge's own low-pass survives on the outermost texels -- and spreads
that correction into the tile as a harmonic (Laplace) field. A harmonic
correction of a high-frequency edge difference dies off within a few texels,
so the tile's interior and its large-scale colour/material layout (coral
patches, transitions) are untouched; what is left at a seam is the
difference of the two edges' low-passes, a soft step instead of cut-off
detail.
"""
import numpy as np
from scipy import fft, ndimage


def _dirichlet(boundary: np.ndarray) -> np.ndarray:
    """Harmonic field on an n x n grid with the given values on its outer ring."""
    n = boundary.shape[0]
    m = n - 2
    rhs = np.zeros((m, m), np.float64)
    rhs[0, :] += boundary[0, 1:-1]
    rhs[-1, :] += boundary[-1, 1:-1]
    rhs[:, 0] += boundary[1:-1, 0]
    rhs[:, -1] += boundary[1:-1, -1]
    k = np.arange(1, m + 1)
    lam = 2 - 2 * np.cos(np.pi * k / (m + 1))
    s = fft.dstn(rhs, type=1)
    s /= lam[:, None] + lam[None, :]
    out = boundary.astype(np.float64).copy()
    out[1:-1, 1:-1] = fft.idstn(s, type=1)
    return out


def ring(t: np.ndarray) -> np.ndarray:
    """Copy of t with everything but the outer ring zeroed."""
    r = np.zeros_like(t)
    r[0], r[-1], r[:, 0], r[:, -1] = t[0], t[-1], t[:, 0], t[:, -1]
    return r


EDGES = (np.s_[0, :], np.s_[-1, :], np.s_[:, 0], np.s_[:, -1])


def edge_mean(tiles: list[np.ndarray]) -> np.ndarray:
    """Mean colour of the outer rings of a set of tiles."""
    return np.mean([t[e].mean(axis=0) for t in tiles for e in EDGES], axis=0)


def harmonize(tile: np.ndarray, sigma: float, mu: np.ndarray | None = None,
              tau: float = 30.0) -> np.ndarray:
    """tile: n x n x c float. Outer ring replaced by its own low-pass along
    the edge (Gaussian, sigma texels), the difference spread harmonically.

    With mu, the low-pass is also pulled towards the colour mu wherever it
    is within about tau of it: brightness drift between tiles of one
    material is flattened at the edges, a different material (coral, sand,
    a transition) is left alone."""
    t = tile.astype(np.float64)
    target = t.copy()
    for e in EDGES:
        lp = ndimage.gaussian_filter1d(t[e], sigma, axis=0, mode="nearest")
        if mu is not None:
            w = np.exp(-(np.linalg.norm(lp - mu, axis=1, keepdims=True) / tau) ** 2)
            lp += w * (mu - lp)
        target[e] = lp
    out = t.copy()
    for c in range(t.shape[2]):
        out[..., c] += _dirichlet(ring(target[..., c] - t[..., c]))
    return out


if __name__ == "__main__":
    # self-check: a harmonic field reproduces its boundary and is smooth inside
    rng = np.random.default_rng(0)
    b = ring(rng.normal(size=(64, 64)))
    h = _dirichlet(b)
    lap = 4 * h[1:-1, 1:-1] - h[:-2, 1:-1] - h[2:, 1:-1] - h[1:-1, :-2] - h[1:-1, 2:]
    assert np.abs(lap).max() < 1e-9 and np.allclose(ring(h), b)
    t = rng.normal(size=(64, 64, 1)) * 10
    o = harmonize(t, 4.0)
    assert np.abs(np.diff(o[:, 0, 0])).mean() < np.abs(np.diff(t[:, 0, 0])).mean() / 3
    print("ok")
