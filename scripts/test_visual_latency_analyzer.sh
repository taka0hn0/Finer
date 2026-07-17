#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
ffmpeg="${FFMPEG:-$(command -v ffmpeg || true)}"
ffprobe="${FFPROBE:-$(command -v ffprobe || true)}"
if [[ -z "$ffmpeg" || -z "$ffprobe" ]]; then
    print -u2 -- "ffmpeg and ffprobe are required"
    exit 1
fi

temp_root="$(mktemp -d "${TMPDIR:-/tmp}/finer-visual-analyzer.XXXXXX")"
trap 'rm -rf "$temp_root"' EXIT
video="$temp_root/synthetic.mov"
vfr_video="$temp_root/synthetic-vfr.mov"
filter="drawbox=x=10:y=10:w=20:h=20:color=red:t=fill:enable='lt(t,0.5)',drawbox=x=10:y=10:w=20:h=20:color=lime:t=fill:enable='gte(t,0.5)',drawbox=x=100:y=20:w=50:h=40:color=white:t=fill:enable='gte(t,0.7)'"

"$ffmpeg" -hide_banner -loglevel error -y \
    -f lavfi -i 'color=c=black:s=200x100:r=60:d=1' \
    -vf "$filter" \
    -c:v libx264 -pix_fmt yuv420p "$video"

"$ffmpeg" -hide_banner -loglevel error -y \
    -f lavfi -i 'color=c=black:s=200x100:r=60:d=1' \
    -vf "$filter,mpdecimate" \
    -fps_mode vfr -c:v libx264 -pix_fmt yuv420p "$vfr_video"

analyze() {
    /usr/bin/python3 "$repo_root/scripts/analyze_visual_latency.py" "$1" \
        --region-width 200 \
        --region-height 100 \
        --marker-x 10 \
        --marker-y 10 \
        --marker-size 20 \
        --sample-step 2 \
        --changed-samples 64 \
        --ffmpeg "$ffmpeg" \
        --ffprobe "$ffprobe"
}

result="$(analyze "$video")"
vfr_result="$(analyze "$vfr_video")"
latency_ms="$(jq -r '.latency_ms' <<<"$result")"
decoded_frames="$(jq -r '.decoded_frames' <<<"$result")"
vfr_latency_ms="$(jq -r '.latency_ms' <<<"$vfr_result")"
vfr_decoded_frames="$(jq -r '.decoded_frames' <<<"$vfr_result")"

awk -v latency="$latency_ms" -v vfr_latency="$vfr_latency_ms" 'BEGIN {
    if (latency < 199.9 || latency > 200.1 || vfr_latency < 199.9 || vfr_latency > 200.1) {
        printf "Unexpected synthetic latency: cfr=%.6fms vfr=%.6fms\n", latency, vfr_latency > "/dev/stderr"
        exit 1
    }
}'
if [[ "$decoded_frames" != 60 ]]; then
    print -u2 -- "Unexpected decoded frame count: $decoded_frames"
    exit 1
fi
if [[ "$vfr_decoded_frames" != 3 ]]; then
    print -u2 -- "Unexpected VFR decoded frame count: $vfr_decoded_frames"
    exit 1
fi

print -- "Visual latency analyzer test passed: latency_ms=$latency_ms cfr_frames=$decoded_frames vfr_frames=$vfr_decoded_frames"
