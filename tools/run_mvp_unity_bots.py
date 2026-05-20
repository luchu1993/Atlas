#!/usr/bin/env python3
"""Launch N Unity standalone-player bots against the MVP cluster.

Each bot is a separate process running `<player.exe> -batchmode -nographics
-atlas-bot <idx> -atlas-bot-duration <sec>`. Bots are staggered to avoid the
LoginApp per-IP rate limit (default 5/60s); for >5 bots, restart the cluster
with `--login-rate-limit-per-ip 0` (run_world_stress.py / run_mvp_cluster
flag) or bump the window-size threshold.

Per-bot logs land under `out/mvp-unity-bots/bot_<idx>.log`.
"""
import argparse
import platform
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT_DIR = REPO_ROOT / "out" / "mvp-unity-bots"


def default_player() -> Path:
    if platform.system() == "Windows":
        return REPO_ROOT / "out" / "mvp-unity" / "windows" / "AtlasMvp.exe"
    if platform.system() == "Darwin":
        return REPO_ROOT / "out" / "mvp-unity" / "macos" / "AtlasMvp.app" / "Contents" / "MacOS" / "AtlasMvp"
    return REPO_ROOT / "out" / "mvp-unity" / "linux" / "AtlasMvp.x86_64"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-n", "--count", type=int, default=4,
                   help="number of bot processes to spawn (default: 4)")
    p.add_argument("--duration", type=float, default=60.0,
                   help="seconds each bot stays connected (default: 60)")
    p.add_argument("--pattern", choices=("random", "pingpong"), default="random",
                   help="bot movement pattern; pingpong walks ±X to cross BSP x=0 boundary")
    p.add_argument("--stagger-ms", type=int, default=400,
                   help="delay between successive bot launches (default: 400)")
    p.add_argument("--player",
                   help=f"path to AtlasMvp player exe (default: {default_player()})")
    p.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR,
                   help=f"directory for per-bot log files (default: {DEFAULT_OUT_DIR})")
    p.add_argument("--start-index", type=int, default=0,
                   help="first bot index (lets you run multiple driver instances)")
    p.add_argument("--timeout", type=float, default=0.0,
                   help="wall-clock timeout in seconds, 0 = wait forever")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    player = Path(args.player).expanduser().resolve() if args.player else default_player()
    if not player.is_file():
        print(f"[run_mvp_unity_bots] player not found: {player}", file=sys.stderr)
        print("[run_mvp_unity_bots] build it first via tools/bin/build_mvp_unity.bat",
              file=sys.stderr)
        return 1

    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.count <= 0:
        print("[run_mvp_unity_bots] --count must be > 0", file=sys.stderr)
        return 2

    procs: list[tuple[int, subprocess.Popen, Path]] = []
    for i in range(args.count):
        idx = args.start_index + i
        idx_str = f"{idx:04d}"
        log_path = out_dir / f"bot_{idx_str}.log"
        cmd = [
            str(player),
            "-batchmode",
            "-nographics",
            "-atlas-bot", idx_str,
            "-atlas-bot-duration", f"{args.duration:.1f}",
            "-atlas-bot-pattern", args.pattern,
            "-logFile", str(log_path),
        ]
        print(f"[run_mvp_unity_bots] [{idx_str}] {' '.join(cmd)}")
        # stdout/stderr go to the log file via -logFile; subprocess channels
        # are merged to DEVNULL so the wrapper terminal stays clean.
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append((idx, p, log_path))
        if i + 1 < args.count and args.stagger_ms > 0:
            time.sleep(args.stagger_ms / 1000.0)

    print(f"[run_mvp_unity_bots] {len(procs)} bot(s) launched; waiting for exit")

    start = time.monotonic()
    while procs:
        if args.timeout > 0 and (time.monotonic() - start) > args.timeout:
            print(f"[run_mvp_unity_bots] timeout {args.timeout}s reached; "
                  f"terminating {len(procs)} bot(s)")
            for _, p, _ in procs:
                if p.poll() is None:
                    p.terminate()
            break
        remaining: list[tuple[int, subprocess.Popen, Path]] = []
        for idx, p, log_path in procs:
            rc = p.poll()
            if rc is None:
                remaining.append((idx, p, log_path))
                continue
            tag = "ok" if rc == 0 else f"rc={rc}"
            print(f"[run_mvp_unity_bots] [{idx:04d}] exited ({tag}) — log: {log_path}")
        procs = remaining
        if procs:
            time.sleep(0.5)

    print("[run_mvp_unity_bots] all bots exited")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("[run_mvp_unity_bots] interrupted; bots may still be alive — kill manually",
              file=sys.stderr)
        sys.exit(130)
