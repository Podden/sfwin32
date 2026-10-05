"""Play the opening of every mission unattended, dumping textures and 2D art.

Rewrites DAT/SAVE/FIGHTIN.DES per mission (the handoff file DESKTOP.EXE
would write), cycles the four cockpits, runs fightport with --dump and a
look-around script, and restores the original FIGHTIN.DES at the end.

    python tools/cover_missions.py [--mode fight3] [--only m00101,...]
"""
import argparse
import csv
import re
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "game" / "SF"
EXE = ROOT / "port" / "build" / "Release" / "fightport.exe"
RUNS = ROOT / "runs" / "cover"


def dramaturgy(mission_file: Path) -> str:
    text = mission_file.read_text(encoding="cp850", errors="replace")
    m = re.search(r"^\s*Dramaturgy\s*=\s*(\S+)", text, re.M | re.I)
    return m.group(1) if m else mission_file.stem.lower().replace("m", "s", 1)


def fightin(template: str, mission: str, drama: str, cockpit: int) -> str:
    out = re.sub(r"^(Mission\s*=\s*).*$", rf"\g<1>{mission}", template, flags=re.M)
    out = re.sub(r"^(Dramaturgy\s*=\s*).*$", rf"\g<1>{drama}", out, flags=re.M)
    # only the [Ship] section's cockpit line, not [Display] Cockpit = 0
    out = re.sub(r"(\[Ship\][^\[]*?cockpit\s*=\s*)\d+", rf"\g<1>{cockpit:03d}", out,
                 flags=re.I)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="fight3")
    ap.add_argument("--only", default="")
    ap.add_argument("--seconds", type=int, default=80)
    ap.add_argument("--dump", default="dump")
    args = ap.parse_args()

    save = GAME / "DAT" / "SAVE" / "FIGHTIN.DES"
    backup = save.with_suffix(".DES.orig")
    if not backup.exists():
        shutil.copy2(save, backup)
    template = backup.read_text(encoding="cp850")

    missions = sorted((GAME / "DAT" / "MISSION").glob("M*.DES"))
    if args.only:
        wanted = {m.lower() for m in args.only.split(",")}
        missions = [m for m in missions if m.stem.lower() in wanted]

    RUNS.mkdir(parents=True, exist_ok=True)
    log = RUNS / "cover.csv"
    with log.open("a", newline="") as fh:
        w = csv.writer(fh)
        for i, mf in enumerate(missions):
            mission = mf.stem.lower()
            drama = dramaturgy(mf)
            save.write_text(fightin(template, mission, drama, i % 4), encoding="cp850")
            t0 = time.time()
            try:
                p = subprocess.run(
                    [str(EXE), "--assets", str(GAME), "--mode", args.mode,
                     "--script", str(ROOT / "runs" / "cover.txt"),
                     "--shotdir", str(RUNS / mission),
                     "--dump", str(ROOT / args.dump)],
                    cwd=RUNS, timeout=args.seconds,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                code = p.returncode
            except subprocess.TimeoutExpired:
                subprocess.run(["taskkill", "/F", "/IM", "fightport.exe"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                code = "timeout"
            logname = "fight3.log" if args.mode == "fight3" else "fight.log"
            src_log = RUNS / logname
            if src_log.exists():
                shutil.copy2(src_log, RUNS / f"{mission}.log")
            w.writerow([mission, drama, i % 4, code, round(time.time() - t0, 1)])
            fh.flush()
            print(mission, drama, code, flush=True)

    shutil.copy2(backup, save)


if __name__ == "__main__":
    main()
