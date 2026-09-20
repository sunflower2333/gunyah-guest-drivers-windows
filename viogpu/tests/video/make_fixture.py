#!/usr/bin/env python3
"""Generate a bounded canonical clip plus independently decoded reference hash."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
coded = args.output / "canonical-12frames.h264"
raw = args.output / "reference-12frames.nv12"
subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30",
                "-frames:v", "12", "-an", "-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p",
                "-crf", "18", "-x264-params", "aud=1:keyint=12:bframes=0", "-f", "h264", str(coded)], check=True)
subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(coded), "-pix_fmt", "nv12", "-f", "rawvideo", str(raw)], check=True)
decoded = raw.read_bytes()
if len(decoded) != 320 * 240 * 3 // 2 * 12:
    raise SystemExit("reference decoder did not produce exactly 12 NV12 frames")
manifest = {"width": 320, "height": 240, "fps": 30, "frames": 12,
            "encoded_sha256": hashlib.sha256(coded.read_bytes()).hexdigest(),
            "decoded_sha256": hashlib.sha256(decoded).hexdigest(),
            "reference": "FFmpeg CPU H.264 decode; device result must match exactly"}
(args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
