#!/usr/bin/env python3
"""Measure marker-to-visible-change latency in a Finer benchmark recording."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path


@dataclass(frozen=True)
class Geometry:
    width: int
    height: int
    marker_left: int
    marker_top: int
    marker_right: int
    marker_bottom: int
    exclusion_left: int
    exclusion_top: int
    exclusion_right: int
    exclusion_bottom: int


def run_json(command: list[str]) -> dict:
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    return json.loads(completed.stdout)


def scaled_geometry(
    video_width: int,
    video_height: int,
    region_width: int,
    region_height: int,
    marker_x: int,
    marker_y: int,
    marker_size: int,
    exclusion_padding: int,
) -> Geometry:
    scale_x = video_width / region_width
    scale_y = video_height / region_height
    left = round(marker_x * scale_x)
    top = round(marker_y * scale_y)
    right = round((marker_x + marker_size) * scale_x)
    bottom = round((marker_y + marker_size) * scale_y)
    padding_x = round(exclusion_padding * scale_x)
    padding_y = round(exclusion_padding * scale_y)
    return Geometry(
        width=video_width,
        height=video_height,
        marker_left=max(0, left),
        marker_top=max(0, top),
        marker_right=min(video_width, right),
        marker_bottom=min(video_height, bottom),
        exclusion_left=max(0, left - padding_x),
        exclusion_top=max(0, top - padding_y),
        exclusion_right=min(video_width, right + padding_x),
        exclusion_bottom=min(video_height, bottom + padding_y),
    )


def marker_average(frame: bytes, geometry: Geometry) -> tuple[float, float, float]:
    total_r = total_g = total_b = samples = 0
    step = max(1, min(geometry.marker_right - geometry.marker_left,
                      geometry.marker_bottom - geometry.marker_top) // 12)
    row_stride = geometry.width * 3
    for y in range(geometry.marker_top, geometry.marker_bottom, step):
        for x in range(geometry.marker_left, geometry.marker_right, step):
            offset = y * row_stride + x * 3
            total_r += frame[offset]
            total_g += frame[offset + 1]
            total_b += frame[offset + 2]
            samples += 1
    if samples == 0:
        raise ValueError("marker rectangle contains no pixels")
    return total_r / samples, total_g / samples, total_b / samples


def marker_state(rgb: tuple[float, float, float]) -> str:
    red, green, blue = rgb
    if red > 100 and red > green * 1.5 and red > blue * 1.2:
        return "red"
    if green > 100 and green > red * 1.3 and green > blue * 1.2:
        return "green"
    return "other"


def changed_sample_count(
    baseline: bytes,
    candidate: bytes,
    geometry: Geometry,
    sample_step: int,
    channel_threshold: int,
    stop_after: int,
) -> int:
    changed = 0
    row_stride = geometry.width * 3
    for y in range(0, geometry.height, sample_step):
        for x in range(0, geometry.width, sample_step):
            if (geometry.exclusion_left <= x < geometry.exclusion_right
                    and geometry.exclusion_top <= y < geometry.exclusion_bottom):
                continue
            offset = y * row_stride + x * 3
            if (abs(candidate[offset] - baseline[offset]) >= channel_threshold
                    or abs(candidate[offset + 1] - baseline[offset + 1]) >= channel_threshold
                    or abs(candidate[offset + 2] - baseline[offset + 2]) >= channel_threshold):
                changed += 1
                if changed >= stop_after:
                    return changed
    return changed


def parse_rate(value: str) -> float:
    if not value or value == "0/0":
        return 0.0
    return float(Fraction(value))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("video", type=Path)
    parser.add_argument("--region-width", type=int, required=True)
    parser.add_argument("--region-height", type=int, required=True)
    parser.add_argument("--marker-x", type=int, required=True,
                        help="marker X relative to the capture rectangle")
    parser.add_argument("--marker-y", type=int, required=True,
                        help="marker Y relative to the capture rectangle")
    parser.add_argument("--marker-size", type=int, required=True)
    parser.add_argument("--sample-step", type=int, default=4)
    parser.add_argument("--channel-threshold", type=int, default=32)
    parser.add_argument("--changed-samples", type=int, default=160)
    parser.add_argument("--exclusion-padding", type=int, default=6)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()

    if not args.video.is_file():
        parser.error(f"video does not exist: {args.video}")
    for name in ("region_width", "region_height", "marker_size", "sample_step",
                 "channel_threshold", "changed_samples"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")

    probe = run_json([
        args.ffprobe,
        "-v", "error",
        "-select_streams", "v:0",
        "-show_frames",
        "-show_entries",
        "stream=width,height,avg_frame_rate,r_frame_rate:frame=best_effort_timestamp_time",
        "-of", "json",
        str(args.video),
    ])
    streams = probe.get("streams", [])
    frames = probe.get("frames", [])
    if len(streams) != 1 or not frames:
        raise RuntimeError("recording has no decodable video stream or frames")
    stream = streams[0]
    width = int(stream["width"])
    height = int(stream["height"])
    timestamps = [float(frame["best_effort_timestamp_time"]) for frame in frames]
    geometry = scaled_geometry(
        width,
        height,
        args.region_width,
        args.region_height,
        args.marker_x,
        args.marker_y,
        args.marker_size,
        args.exclusion_padding,
    )

    decoder = subprocess.Popen([
        args.ffmpeg,
        "-v", "error",
        "-i", str(args.video),
        "-map", "0:v:0",
        "-fps_mode", "passthrough",
        "-pix_fmt", "rgb24",
        "-f", "rawvideo",
        "-",
    ], stdout=subprocess.PIPE)
    if decoder.stdout is None:
        raise RuntimeError("failed to open ffmpeg output")

    frame_size = width * height * 3
    last_red: bytes | None = None
    marker_timestamp: float | None = None
    response_timestamp: float | None = None
    response_changed_samples = 0
    decoded_frames = 0

    try:
        for timestamp in timestamps:
            frame = decoder.stdout.read(frame_size)
            if len(frame) != frame_size:
                raise RuntimeError(
                    f"short decoded frame: expected={frame_size} actual={len(frame)}")
            decoded_frames += 1
            state = marker_state(marker_average(frame, geometry))
            if marker_timestamp is None:
                if state == "red":
                    last_red = frame
                    continue
                if state != "green":
                    continue
                if last_red is None:
                    raise RuntimeError("green marker appeared before a red baseline frame")
                marker_timestamp = timestamp

            if response_timestamp is not None:
                continue

            changed = changed_sample_count(
                last_red,
                frame,
                geometry,
                args.sample_step,
                args.channel_threshold,
                args.changed_samples,
            )
            if changed >= args.changed_samples:
                response_timestamp = timestamp
                response_changed_samples = changed
    finally:
        decoder.stdout.close()
        decoder.wait()

    if decoder.returncode != 0:
        raise RuntimeError(f"ffmpeg exited with status {decoder.returncode}")
    if marker_timestamp is None:
        raise RuntimeError("recording contains no red-to-green marker transition")
    if response_timestamp is None:
        raise RuntimeError("no visible change exceeded the response threshold")

    result = {
        "video": str(args.video),
        "width": width,
        "height": height,
        "frames": len(timestamps),
        "decoded_frames": decoded_frames,
        "nominal_frame_rate": parse_rate(stream.get("r_frame_rate", "0/0")),
        "average_frame_rate": parse_rate(stream.get("avg_frame_rate", "0/0")),
        "marker_pts_seconds": marker_timestamp,
        "response_pts_seconds": response_timestamp,
        "latency_ms": (response_timestamp - marker_timestamp) * 1000.0,
        "response_changed_samples": response_changed_samples,
        "sample_step": args.sample_step,
        "channel_threshold": args.channel_threshold,
        "changed_samples_threshold": args.changed_samples,
    }
    json.dump(result, sys.stdout, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError, ValueError) as error:
        print(f"analyze_visual_latency.py: {error}", file=sys.stderr)
        raise SystemExit(1)
