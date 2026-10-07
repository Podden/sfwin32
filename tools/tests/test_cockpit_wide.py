"""Tests for cockpit_wide.style_dirs -- the per-style output routing that keeps
building one --style from overwriting the final asset of another (AIB-228).

cockpit_wide imports numpy/PIL/scipy and the sibling outpaint_cockpit at module
load time. style_dirs itself only needs pathlib, so we stub those heavy deps in
sys.modules before importing the module under test; the test stays runnable
without the full image pipeline installed.
"""
import importlib.util
import sys
import types
from pathlib import Path
from unittest.mock import MagicMock

import pytest

TOOLS = Path(__file__).resolve().parent.parent


def _stub(name: str, **attrs) -> types.ModuleType:
    mod = types.ModuleType(name)
    for k, v in attrs.items():
        setattr(mod, k, v)
    sys.modules.setdefault(name, mod)
    return mod


def _load_cockpit_wide():
    # numpy/PIL/scipy are only exercised by the image pipeline, not by
    # style_dirs; MagicMock satisfies the module-level np.array(...) constants
    # and the import-time attribute access without pulling the real packages.
    sys.modules.setdefault("numpy", MagicMock(name="numpy"))
    pil = _stub("PIL")
    pil.Image = sys.modules.setdefault("PIL.Image", MagicMock(name="PIL.Image"))
    scipy = _stub("scipy")
    scipy.ndimage = sys.modules.setdefault("scipy.ndimage", MagicMock(name="scipy.ndimage"))
    # cockpit_wide does `sys.path.insert(parent); from outpaint_cockpit import ...`
    sys.path.insert(0, str(TOOLS))
    _stub(
        "outpaint_cockpit",
        SECRET=None, generate=None, hd_art=None, log_spend=None, pairs=None,
    )
    spec = importlib.util.spec_from_file_location(
        "cockpit_wide", TOOLS / "cockpit_wide.py"
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


cockpit_wide = _load_cockpit_wide()
style_dirs = cockpit_wide.style_dirs


@pytest.fixture
def roots(tmp_path):
    return tmp_path / "hd_v2", tmp_path / "runs"


def _final(out_dir: Path, name: str = "c01") -> Path:
    return out_dir / f"{name}.png"


def test_default_style_keeps_canonical_game_path(roots):
    hd, runs = roots
    out_dir, _ = style_dirs(hd, runs, "original")
    # The game consumes <hd>/cockpit/<name>.png -- must stay unchanged.
    assert out_dir == hd / "cockpit"


def test_different_styles_write_separate_targets(roots):
    hd, runs = roots
    out_orig, _ = style_dirs(hd, runs, "original")
    out_pano, _ = style_dirs(hd, runs, "panorama")
    assert out_orig != out_pano
    assert _final(out_orig) != _final(out_pano)
    assert out_pano == hd / "cockpit" / "panorama"


def test_building_one_style_does_not_overwrite_the_other(roots, tmp_path):
    """Simulate two runs with different --style on the same input: the asset
    written by the first run must survive the second."""
    hd, runs = roots
    out_orig, _ = style_dirs(hd, runs, "original")
    out_pano, _ = style_dirs(hd, runs, "panorama")
    out_orig.mkdir(parents=True, exist_ok=True)
    out_pano.mkdir(parents=True, exist_ok=True)

    asset_orig = _final(out_orig)
    asset_orig.write_text("ORIGINAL")

    asset_pano = _final(out_pano)
    asset_pano.write_text("PANORAMA")

    # Neither run clobbered the other's final asset.
    assert asset_orig.read_text() == "ORIGINAL"
    assert asset_pano.read_text() == "PANORAMA"
    assert asset_orig != asset_pano
