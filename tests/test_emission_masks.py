"""Tests for tools/emission_masks.py.

Focus: a group of same-hash textures with mismatched sizes must be skipped
(not crash the whole batch), while a same-size group still yields a mask.
"""
import importlib.util
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

spec = importlib.util.spec_from_file_location("emission_masks", TOOLS / "emission_masks.py")
emission_masks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(emission_masks)


def _black_with_light(size: int) -> np.ndarray:
    """A mostly-black RGB atlas with a tiny bright spot (a detectable light)."""
    a = np.zeros((size, size, 3), np.uint8)
    a[4:7, 4:7] = 255
    return a


def _write_tga(path: Path, arr: np.ndarray) -> None:
    Image.fromarray(arr, "RGB").save(path)


@pytest.fixture
def dump(tmp_path):
    tex = tmp_path / "dump" / "tex"
    tex.mkdir(parents=True)
    # Same hash prefix, DIFFERENT sizes -> must be skipped.
    _write_tga(tex / "aaaa_0.tga", _black_with_light(256))
    _write_tga(tex / "aaaa_1.tga", _black_with_light(128))
    # Same hash prefix, SAME size -> mask must be produced.
    _write_tga(tex / "bbbb_0.tga", _black_with_light(256))
    _write_tga(tex / "bbbb_1.tga", _black_with_light(256))
    return tmp_path


def _run(dump_root, monkeypatch):
    hd = dump_root / "hd"
    monkeypatch.setattr(sys, "argv", [
        "emission_masks.py",
        "--dump", str(dump_root / "dump"),
        "--hd", str(hd),
    ])
    emission_masks.main()
    return hd / "emit"


def test_mismatched_sizes_group_is_skipped_batch_continues(dump, monkeypatch, capsys):
    emit = _run(dump, monkeypatch)
    out = capsys.readouterr().out
    # The defective group is reported and skipped...
    assert "skip aaaa mismatched sizes" in out
    assert not (emit / "aaaa.png").exists()
    # ...while the healthy group is still processed (batch did not die).
    assert (emit / "bbbb.png").exists()


def test_same_size_group_yields_mask(dump, monkeypatch):
    emit = _run(dump, monkeypatch)
    assert (emit / "bbbb.png").exists()
