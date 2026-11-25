#!/usr/bin/env python3
"""Tempo decisions: incremental, spread ACF vs the old batch ACF, on edm/.

The firmware computes each tempo decision from exact incremental ACF tables
(blinky-things/audio/SlidingAcf.h) and spreads it over one acfPeriodMs cycle.
The harness's --acf-trace recomputes every decision with the old batch code
(tests/tracker/acf_reference.h) on the same snapshot and logs both. This runs
it over the corpus at the device's level and reports, per track and overall:

  decisions            decisions compared
  period/mult/src/valid share identical (the integer choices)
  dPeriodF             max |periodF difference|, frames
  dPhase               max circular |phase target difference| at the snapshot
  dAccent              max |accent bin difference|
  lag                  frames between the snapshot and the applied decision

and lists every decision where an integer choice differs. Fails (exit 1) if
any integer choice differs or a continuous value is off by more than
--max-period-f / --max-phase: the decisions must be the old ones.

    python tests/tracker/acf_equivalence.py --harness build/tracker_harness [--tracks ...]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_tracker_eval import CORPUS, DEVICE_LEVEL_DB, decode, find_ffmpeg  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True)
    ap.add_argument("--tracks", nargs="*")
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--ffmpeg")
    ap.add_argument("--level-db", type=float, default=DEVICE_LEVEL_DB)
    ap.add_argument("--max-period-f", type=float, default=1e-3, help="max |periodF difference| (frames)")
    ap.add_argument("--max-phase", type=float, default=1e-4, help="max |phase target difference| (cycles)")
    args = ap.parse_args()

    ffmpeg = find_ffmpeg(args.ffmpeg)
    stems = args.tracks or sorted(p.name[:-len(".onsets_consensus.json")]
                                  for p in CORPUS.glob("*.onsets_consensus.json"))
    tot = {"n": 0, "same": 0, "dpf": 0.0, "dph": 0.0, "dacc": 0.0, "dstr": 0.0}
    diffs: list[str] = []
    lags: set[int] = set()
    print(f"{'track':32s} {'decisions':>9s} {'identical':>9s} {'dPeriodF':>9s} {'dPhase':>9s} "
          f"{'dAccent':>9s} {'dStrength':>9s} {'lag':>5s}")
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        for stem in stems:
            pcm = tmpd / f"{stem}.pcm"
            decode(ffmpeg, CORPUS / f"{stem}.mp3", pcm, args.level_db)
            trace = tmpd / f"{stem}.acf.csv"
            subprocess.run([args.harness, str(pcm), str(tmpd / "out.csv"), str(args.fps), "0.25", "1",
                            f"--acf-trace={trace}"], check=True)
            d = np.genfromtxt(trace, delimiter=",", names=True)
            ints = ["valid", "period", "mult", "src"]
            same = np.ones(len(d), dtype=bool)
            for k in ints:
                same &= d[f"new_{k}"] == d[f"ref_{k}"]
            both = same & (d["new_valid"] == 1)
            dpf = float(np.max(np.abs(d["new_period_f"] - d["ref_period_f"])[both], initial=0.0))
            ph = np.abs(d["new_phase"] - d["ref_phase"])[both]
            dph = float(np.max(np.minimum(ph, 1.0 - ph), initial=0.0))
            dacc = float(np.max(np.abs(d["new_accent"] - d["ref_accent"])[both], initial=0.0))
            dstr = float(np.max(np.abs(d["new_strength"] - d["ref_strength"]), initial=0.0))
            lag = sorted({int(x) for x in d["apply_frame"] - d["snap_frame"]})
            lags.update(lag)
            print(f"{stem:32s} {len(d):9d} {same.mean():9.4f} {dpf:9.2e} {dph:9.2e} {dacc:9.2e} "
                  f"{dstr:9.2e} {'/'.join(map(str, lag)):>5s}", flush=True)
            for row in d[~same]:
                diffs.append(f"  {stem} frame {int(row['snap_frame'])}: new period {int(row['new_period'])} "
                             f"x{int(row['new_mult'])} src {int(row['new_src'])} (strength "
                             f"{row['new_strength']:.6f}) vs old {int(row['ref_period'])} x{int(row['ref_mult'])} "
                             f"src {int(row['ref_src'])} ({row['ref_strength']:.6f})")
            tot["n"] += len(d)
            tot["same"] += int(same.sum())
            tot["dpf"] = max(tot["dpf"], dpf)
            tot["dph"] = max(tot["dph"], dph)
            tot["dacc"] = max(tot["dacc"], dacc)
            tot["dstr"] = max(tot["dstr"], dstr)
    print()
    print(f"ALL: {tot['n']} decisions, {tot['same']} identical ({tot['same'] / max(tot['n'], 1):.5f}); "
          f"max dPeriodF {tot['dpf']:.2e} frames, max dPhase {tot['dph']:.2e}, max dAccent {tot['dacc']:.2e}, "
          f"max dStrength {tot['dstr']:.2e}; applied {'/'.join(map(str, sorted(lags)))} frames after the snapshot")
    if diffs:
        print("Decisions whose integer choice differs:")
        print("\n".join(diffs))
    failed = bool(diffs) or tot["dpf"] > args.max_period_f or tot["dph"] > args.max_phase
    if failed:
        print("[GATE FAIL] the spread, table-driven decisions must equal the old batch decisions")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
