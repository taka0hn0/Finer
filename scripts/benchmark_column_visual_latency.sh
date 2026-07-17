#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
result_root="${FINDER_VIM_RESULT_ROOT:-$repo_root/.build/benchmark-results}"
helper="${FINDER_VIM_HELPER:-$HOME/.local/libexec/finder-vim/finder_ax_step}"
capture_helper="${FINER_VISUAL_CAPTURE_HELPER:-$repo_root/.build/finer_visual_capture}"
content_profile="${FINDER_VIM_BENCHMARK_PROFILE:-empty-files}"
iterations="${1:-10}"
counts_string="${FINDER_VIM_BENCHMARK_COUNTS:-10 1000 10000}"
counts=("${(@s: :)counts_string}")
ffmpeg="${FFMPEG:-$(command -v ffmpeg || true)}"
ffprobe="${FFPROBE:-$(command -v ffprobe || true)}"

window_x=180
window_y=140
window_width=1000
window_height=680
marker_relative_x=12
marker_relative_y=640
marker_size=20
marker_x=$(( window_x + marker_relative_x ))
marker_y=$(( window_y + marker_relative_y ))

if [[ ! "$iterations" =~ '^[1-9][0-9]*$' ]]; then
    print -u2 -- "Iterations must be a positive integer: $iterations"
    exit 64
fi
for count in "${counts[@]}"; do
    if [[ "$count" != 10 && "$count" != 1000 && "$count" != 10000 ]]; then
        print -u2 -- "Unsupported item count: $count"
        exit 64
    fi
done
if [[ ! -x "$helper" ]]; then
    print -u2 -- "Missing executable helper: $helper"
    exit 1
fi
if [[ ! -x "$capture_helper" ]]; then
    print -u2 -- "Missing visual capture helper: $capture_helper"
    print -u2 -- "Run make benchmark-visual-helper."
    exit 1
fi
if [[ -z "$ffmpeg" || -z "$ffprobe" ]]; then
    print -u2 -- "ffmpeg and ffprobe are required"
    exit 1
fi
"$repo_root/scripts/require_benchmark_metrics.sh" "$helper"

profile_marker="$fixture_root/.content-profile"
if [[ ! -r "$profile_marker" || "$(<"$profile_marker")" != "$content_profile" ]]; then
    print -u2 -- "Fixture profile does not match $content_profile: $fixture_root"
    exit 1
fi

run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_root="$result_root/column-visual-$run_id"
videos_root="$run_root/videos"
samples_file="$run_root/samples.tsv"
metrics_file="$run_root/worker.metrics.tsv"
environment_file="$run_root/environment.txt"
mkdir -p "$videos_root"
touch "$samples_file" "$metrics_file" "$environment_file"
truncate -s 0 "$samples_file" "$metrics_file" "$environment_file"

window_id=""
close_test_window() {
    if [[ -z "$window_id" ]]; then
        return
    fi
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript \
        -e 'set windowId to (system attribute "FINDER_VIM_WINDOW_ID") as integer' \
        -e 'tell application "Finder"' \
        -e 'if exists (first Finder window whose id is windowId) then close (first Finder window whose id is windowId)' \
        -e 'end tell' >/dev/null 2>&1 || true
    window_id=""
}

activate_test_window() {
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript \
        -e 'set windowId to (system attribute "FINDER_VIM_WINDOW_ID") as integer' \
        -e 'tell application "Finder"' \
        -e 'set testWindow to first Finder window whose id is windowId' \
        -e 'set index of testWindow to 1' \
        -e 'activate' \
        -e 'end tell' >/dev/null
}

selected_path() {
    /usr/bin/osascript \
        -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' \
        2>/dev/null || true
}

open_test_window() {
    FINDER_VIM_CASE_DIR="$case_dir" /usr/bin/osascript \
        -e 'set casePath to system attribute "FINDER_VIM_CASE_DIR"' \
        -e 'tell application "Finder"' \
        -e 'set targetFolder to POSIX file casePath as alias' \
        -e 'set testWindow to make new Finder window to targetFolder' \
        -e 'set current view of testWindow to column view' \
        -e 'set bounds of testWindow to {180, 140, 1180, 820}' \
        -e 'set selection to {file "00-start.txt" of targetFolder}' \
        -e 'set index of testWindow to 1' \
        -e 'activate' \
        -e 'return id of testWindow' \
        -e 'end tell'
}

wait_for_worker_exit() {
    local socket="$HOME/.local/state/finder-vim/finder_ax_step.sock"
    for ((attempt = 1; attempt <= 40; ++attempt)); do
        if [[ ! -S "$socket" ]]; then
            sleep 0.1
            return 0
        fi
        sleep 0.1
    done
    print -u2 -- "Finer worker did not exit before visual capture"
    return 1
}

prepare_initial_focus() {
    local expected="$case_dir/01-A/"
    for ((attempt = 1; attempt <= 20; ++attempt)); do
        activate_test_window
        "$helper" first >/dev/null 2>&1 || true
        "$helper" hold-start down >/dev/null 2>&1 || true
        sleep 0.1
        if [[ "$(selected_path)" == "$expected" ]]; then
            wait_for_worker_exit
            activate_test_window
            sleep 0.2
            if [[ "$(selected_path)" == "$expected" ]]; then
                return 0
            fi
        fi
        sleep 0.1
    done
    print -u2 -- "Finder did not reach visual benchmark start state: $label iteration $iteration"
    return 1
}

wait_for_metrics_row() {
    local expected_rows="$1"
    local rows
    for ((attempt = 1; attempt <= 40; ++attempt)); do
        rows="$(awk 'END { print (NR > 0 ? NR - 1 : 0) }' "$metrics_file")"
        if (( rows >= expected_rows )); then
            return 0
        fi
        sleep 0.1
    done
    print -u2 -- "Worker metrics did not flush: expected=$expected_rows actual=$rows"
    return 1
}

trap close_test_window EXIT
trap 'close_test_window; exit 130' INT
trap 'close_test_window; exit 143' TERM

commit="$(git -C "$repo_root" rev-parse HEAD)"
dirty=false
if [[ -n "$(git -C "$repo_root" status --porcelain)" ]]; then
    dirty=true
fi
finder_version="$(defaults read /System/Library/CoreServices/Finder.app/Contents/Info CFBundleShortVersionString 2>/dev/null || print unknown)"
karabiner_cli="/Library/Application Support/org.pqrs/Karabiner-Elements/bin/karabiner_cli"
karabiner_version=unknown
if [[ -x "$karabiner_cli" ]]; then
    karabiner_version="$("$karabiner_cli" --version 2>/dev/null || print unknown)"
fi

{
    print -- "run_id=$run_id"
    print -- "commit=$commit"
    print -- "dirty=$dirty"
    print -- "macos_version=$(sw_vers -productVersion)"
    print -- "macos_build=$(sw_vers -buildVersion)"
    print -- "finder_version=$finder_version"
    print -- "karabiner_version=$karabiner_version"
    print -- "hardware_model=$(sysctl -n hw.model)"
    print -- "architecture=$(uname -m)"
    print -- "view=column"
    print -- "content_profile=$content_profile"
    print -- "iterations=$iterations"
    print -- "counts=$counts_string"
    print -- "helper=$helper"
    print -- "capture_helper=$capture_helper"
    print -- "capture_rectangle=$window_x,$window_y,$window_width,$window_height"
    print -- "capture_marker=$marker_relative_x,$marker_relative_y,$marker_size"
    print -- "physical_input=false"
    print -- "karabiner_evaluation=false"
    print -- "marker_precedes_helper_spawn=true"
} > "$environment_file"

print -r -- $'label\titeration\tresult\tselected_path\tmarker_pts_seconds\tresponse_pts_seconds\tvisual_latency_ms\tresponse_changed_samples\tnominal_frame_rate\taverage_frame_rate\tworker_state\tdispatch_ms\tworker_duration_ms\tvisual_minus_dispatch_ms\tvideo' > "$samples_file"
expected_metrics_rows=0

for count in "${counts[@]}"; do
    label="items-$count"
    case_dir="$fixture_root/$label"
    expected_path="$case_dir/01-A/item-00000.txt"
    if [[ ! -f "$case_dir/00-start.txt" || ! -f "$expected_path" ]]; then
        print -u2 -- "Missing fixture: $case_dir"
        exit 1
    fi

    for ((iteration = 1; iteration <= iterations; ++iteration)); do
        window_id="$(open_test_window)"
        prepare_initial_focus

        video="$videos_root/$label-iteration-$iteration.mov"
        analysis="$videos_root/$label-iteration-$iteration.json"
        rm -f "$video" "$analysis"
        FINDER_VIM_METRICS_FILE="$metrics_file" \
        FINDER_VIM_METRICS_LABEL="$label" \
            "$capture_helper" \
                "$video" \
                "$window_x" "$window_y" "$window_width" "$window_height" \
                "$marker_x" "$marker_y" "$marker_size" \
                "$helper" hold-start right

        /usr/bin/python3 "$repo_root/scripts/analyze_visual_latency.py" "$video" \
            --region-width "$window_width" \
            --region-height "$window_height" \
            --marker-x "$marker_relative_x" \
            --marker-y "$marker_relative_y" \
            --marker-size "$marker_size" \
            --ffmpeg "$ffmpeg" \
            --ffprobe "$ffprobe" > "$analysis"

        final_path="$(selected_path)"
        result=pass
        if [[ "$final_path" != "$expected_path" ]]; then
            result=fail
        fi
        marker_pts="$(jq -r '.marker_pts_seconds' "$analysis")"
        response_pts="$(jq -r '.response_pts_seconds' "$analysis")"
        latency_ms="$(jq -r '.latency_ms' "$analysis")"
        changed_samples="$(jq -r '.response_changed_samples' "$analysis")"
        nominal_rate="$(jq -r '.nominal_frame_rate' "$analysis")"
        average_rate="$(jq -r '.average_frame_rate' "$analysis")"
        (( ++expected_metrics_rows ))
        wait_for_metrics_row "$expected_metrics_rows"
        if ! awk -F '\t' 'NR > 1 { position = $17 + 0; dropped = $18 + 0 }
            END { exit !(position > 0 && dropped == 0) }' "$metrics_file"; then
            print -u2 -- "Invalid worker metrics: $label iteration $iteration"
            exit 1
        fi
        worker_state="$(awk -F '\t' 'NR > 1 { value = $4 } END { print value }' "$metrics_file")"
        dispatch_ms="$(awk -F '\t' 'NR > 1 { value = $6 / 1000000 } END { printf "%.6f", value }' "$metrics_file")"
        worker_duration_ms="$(awk -F '\t' 'NR > 1 { value = $7 / 1000000 } END { printf "%.6f", value }' "$metrics_file")"
        visual_minus_dispatch_ms="$(awk -v visual="$latency_ms" -v dispatch="$dispatch_ms" 'BEGIN { printf "%.6f", visual - dispatch }')"
        print -r -- "$label"$'\t'"$iteration"$'\t'"$result"$'\t'"$final_path"$'\t'"$marker_pts"$'\t'"$response_pts"$'\t'"$latency_ms"$'\t'"$changed_samples"$'\t'"$nominal_rate"$'\t'"$average_rate"$'\t'"$worker_state"$'\t'"$dispatch_ms"$'\t'"$worker_duration_ms"$'\t'"$visual_minus_dispatch_ms"$'\t'"$video" >> "$samples_file"

        close_test_window
    done
done

print -- "Samples: $samples_file"
print -- "Worker metrics: $metrics_file"
print -- "Environment: $environment_file"
awk -F '\t' '
NR == 1 { next }
{
    group = $1
    values[group, ++count[group]] = $7 + 0
    visualMinusDispatch[group] += $14 + 0
    if ($3 != "pass") failures[group]++
}
END {
    print "label\tsamples\tvisual_p50_ms\tvisual_p95_ms\tvisual_max_ms\tavg_visual_minus_dispatch_ms\tfailures"
    for (group in count) {
        for (i = 1; i <= count[group]; ++i) sorted[i] = values[group, i]
        for (i = 2; i <= count[group]; ++i) {
            value = sorted[i]
            j = i - 1
            while (j >= 1 && sorted[j] > value) {
                sorted[j + 1] = sorted[j]
                --j
            }
            sorted[j + 1] = value
        }
        p50 = int((count[group] * 50 + 99) / 100)
        p95 = int((count[group] * 95 + 99) / 100)
        printf "%s\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%d\n", group, count[group], sorted[p50], sorted[p95], sorted[count[group]], visualMinusDispatch[group] / count[group], failures[group]
        delete sorted
    }
}' "$samples_file"

if awk -F '\t' 'NR > 1 && $3 != "pass" { exit 1 }' "$samples_file"; then
    exit 0
fi
exit 1
