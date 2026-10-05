"""Upscale and smooth the game's videos: 640x240 @ 10 fps -> 1920x720 @ 30 fps.

    python tools/upscale_video.py [--src DIR] [--out hd/mvi] [--only FILM] [--names D02,D03]

Input is the MVI material already converted to <name>.mp4 + <name>.wav
(Schleichfahrt2's exporter). Per video: frames out with ffmpeg, Real-ESRGAN
(anime-video model, x3) for detail, RIFE v4.6 for two in-between frames per
pair, then H.264. The game shows a film 1:1 as a 640x240 band across the
middle of the screen, so the output keeps that 8:3 shape; it has no sound,
the game plays the film's own. Output goes to <out>/<category>/<name>.mp4,
which fightport draws over that band. Existing outputs are skipped.
Intermediate frames are PNG: the earlier JPEG round trips plus a deband pass
came out as a diagonal hatch over every frame. The longest film needs about
20 GB of temporary space.
"""
import argparse
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(r"D:\Projekte\Schleichfahrt2\extracted\converted\GFX\MVI")
ESRGAN = ROOT / "third_party" / "realesrgan" / "realesrgan-ncnn-vulkan.exe"
RIFE_DIR = ROOT / "third_party" / "rife-ncnn-vulkan-20221029-windows"
RIFE = RIFE_DIR / "rife-ncnn-vulkan.exe"


def run(cmd: list, **kw) -> None:
    subprocess.run([str(c) for c in cmd], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)


def frames(path: Path) -> int:
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=nb_frames", "-of", "csv=p=0",
                          str(path)], capture_output=True, text=True, check=True)
    return int(out.stdout.strip() or 0)


def process(mp4: Path, dst: Path, fps_out: int) -> None:
    n = frames(mp4)
    with tempfile.TemporaryDirectory(dir=ROOT / "runs") as tmp:
        t = Path(tmp)
        raw, up, mid = t / "raw", t / "up", t / "mid"
        for d in (raw, up, mid):
            d.mkdir()
        run(["ffmpeg", "-v", "error", "-i", mp4, "-vsync", "0", raw / "%06d.png"])
        run([ESRGAN, "-i", raw, "-o", up, "-n", "realesr-animevideov3", "-s", "3"])
        # RIFE needs the target count; x3 frames over the same duration
        run([RIFE, "-i", up, "-o", mid, "-m", "rife-v4.6", "-n", n * 3, "-f", "%08d.png"],
            cwd=RIFE_DIR)
        cmd = ["ffmpeg", "-v", "error", "-y", "-framerate", fps_out, "-i", mid / "%08d.png",
               "-an", "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-pix_fmt",
               "yuv420p", "-movflags", "+faststart", dst]
        dst.parent.mkdir(parents=True, exist_ok=True)
        run(cmd)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, default=SRC)
    ap.add_argument("--out", type=Path, default=ROOT / "hd_v2" / "mvi")
    ap.add_argument("--only", default="FILM", help="comma-separated categories")
    ap.add_argument("--names", default="", help="comma-separated video names")
    args = ap.parse_args()
    names = {n.upper() for n in args.names.split(",") if n}
    for cat in args.only.split(","):
        for mp4 in sorted((args.src / cat).glob("*.mp4")):
            if names and mp4.stem.upper() not in names:
                continue
            dst = args.out / cat / mp4.name
            if dst.exists():
                continue
            fps = int(round(float(subprocess.run(
                ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                 "stream=r_frame_rate", "-of", "csv=p=0", str(mp4)],
                capture_output=True, text=True).stdout.strip().split("/")[0])))
            t0 = time.time()
            process(mp4, dst, fps * 3)
            print(f"{cat}/{mp4.name}: {frames(mp4)} frames, {time.time() - t0:.0f} s",
                  flush=True)


if __name__ == "__main__":
    main()
