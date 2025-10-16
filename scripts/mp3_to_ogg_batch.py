#!/usr/bin/env python3
"""
Batch convert all .mp3 files in a directory to .ogg (Opus) using ffmpeg.

Defaults mirror scripts/mp3_to_ogg.sh:
  - codec: libopus
  - bitrate: 16k
  - channels: 1
  - sample rate: 16000 Hz
  - frame duration: 60 ms

Usage:
  python mp3_to_ogg_batch.py --input <input_dir> [--output <output_dir>] [--recursive]
                             [--overwrite] [--ffmpeg ffmpeg]

Examples:
  python mp3_to_ogg_batch.py --input ./music --recursive
  python mp3_to_ogg_batch.py --input ./music --output ./out --overwrite
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import List


def find_mp3_files(root: Path, recursive: bool) -> List[Path]:
    if recursive:
        return [p for p in root.rglob("*.mp3") if p.is_file()]
    return [p for p in root.glob("*.mp3") if p.is_file()]


def ensure_parent_dir(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def convert_one(ffmpeg: str, src: Path, dst: Path, overwrite: bool,
                bitrate: str, channels: int, sample_rate: int, frame_duration: int) -> int:
    ensure_parent_dir(dst)
    cmd = [
        ffmpeg,
        "-hide_banner", "-loglevel", "error",
        "-y" if overwrite else "-n",
        "-i", str(src),
        "-c:a", "libopus",
        "-b:a", str(bitrate),
        "-ac", str(channels),
        "-ar", str(sample_rate),
        "-frame_duration", str(frame_duration),
        str(dst),
    ]
    # Flatten optional -y/-n position
    cmd = [arg for arg in cmd if arg]
    try:
        result = subprocess.run(cmd, check=False)
        return result.returncode
    except FileNotFoundError:
        print(f"ERROR: ffmpeg not found at '{ffmpeg}'.", file=sys.stderr)
        return 127


def main() -> int:
    parser = argparse.ArgumentParser(description="Batch convert .mp3 to .ogg (Opus) with ffmpeg")
    parser.add_argument("--input", "-i", required=True, help="Input directory to scan for .mp3 files")
    parser.add_argument("--output", "-o", default=None, help="Output directory (default: same as input)")
    parser.add_argument("--recursive", "-r", action="store_true", help="Recurse into subdirectories")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing files")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable path (default: ffmpeg)")
    parser.add_argument("--bitrate", default="16k", help="Audio bitrate (default: 16k)")
    parser.add_argument("--channels", type=int, default=1, help="Channels (default: 1)")
    parser.add_argument("--sample_rate", type=int, default=16000, help="Sample rate Hz (default: 16000)")
    parser.add_argument("--frame_duration", type=int, default=60, help="Opus frame duration ms (default: 60)")

    args = parser.parse_args()

    in_dir = Path(args.input).resolve()
    if not in_dir.exists() or not in_dir.is_dir():
        print(f"ERROR: Input directory not found: {in_dir}", file=sys.stderr)
        return 2

    out_dir = Path(args.output).resolve() if args.output else in_dir

    mp3_files = find_mp3_files(in_dir, args.recursive)
    if not mp3_files:
        print("No .mp3 files found.")
        return 0

    print(f"Found {len(mp3_files)} mp3 files under {in_dir}")

    failures = 0
    for src in mp3_files:
        rel = src.relative_to(in_dir)
        dst = (out_dir / rel).with_suffix(".ogg")
        if dst.exists() and not args.overwrite:
            print(f"Skip (exists): {dst}")
            continue
        print(f"Converting: {src} -> {dst}")
        code = convert_one(args.ffmpeg, src, dst, args.overwrite, args.bitrate, args.channels, args.sample_rate, args.frame_duration)
        if code != 0:
            failures += 1
            print(f"Failed ({code}): {src}", file=sys.stderr)

    if failures:
        print(f"Completed with {failures} failures out of {len(mp3_files)} files.", file=sys.stderr)
        return 1

    print("All conversions completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())


