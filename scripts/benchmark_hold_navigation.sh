#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
result_root="${FINDER_VIM_RESULT_ROOT:-$repo_root/.build/benchmark-results}"
helper="${FINDER_VIM_HELPER:-$HOME/.local/libexec/finder-vim/finder_ax_step}"
content_profile="${FINDER_VIM_BENCHMARK_PROFILE:-empty-files}"
preflight_only="${FINDER_VIM_BENCHMARK_PREFLIGHT:-0}"
iterations="${1:-10}"
hold_duration_ms="${FINDER_VIM_HOLD_DURATION_MS:-1000}"
held_threshold_ms="${FINDER_VIM_HELD_THRESHOLD_MS:-150}"
case_dir="$fixture_root/items-1000/01-A"
token_file="${FINDER_VIM_HOLD_TOKEN_FILE:-$HOME/.local/state/finder-vim/finder_down_hold.txt}"

if [[ ! "$iterations" =~ '^[1-9][0-9]*$' ]]; then
    print -u2 -- "Iterations must be a positive integer: $iterations"
    exit 64
fi
if [[ ! "$hold_duration_ms" =~ '^[1-9][0-9]*$' \
    || "$hold_duration_ms" -lt 250 || "$hold_duration_ms" -gt 5000 ]]; then
    print -u2 -- "Hold duration must be between 250 and 5000ms: $hold_duration_ms"
    exit 64
fi
if [[ ! "$held_threshold_ms" =~ '^[1-9][0-9]*$' \
    || "$held_threshold_ms" -gt 1000 ]]; then
    print -u2 -- "Held threshold must be between 1 and 1000ms: $held_threshold_ms"
    exit 64
fi
if [[ ! -x "$helper" ]]; then
    print -u2 -- "Missing executable helper: $helper"
    print -u2 -- "Run make install or set FINDER_VIM_HELPER."
    exit 1
fi
case "$content_profile" in
    empty-files|realistic-mixed) ;;
    *)
        print -u2 -- "Unsupported content profile: $content_profile"
        exit 64
        ;;
esac
if [[ "$preflight_only" != 0 && "$preflight_only" != 1 ]]; then
    print -u2 -- "FINDER_VIM_BENCHMARK_PREFLIGHT must be 0 or 1: $preflight_only"
    exit 64
fi
if [[ ! -r "$fixture_root/.content-profile" \
    || "$(<"$fixture_root/.content-profile")" != "$content_profile" ]]; then
    print -u2 -- "Fixture profile does not match $content_profile: $fixture_root"
    print -u2 -- "Run the matching benchmark fixture target."
    exit 1
fi
typeset -a ordered_items=("${case_dir}"/*(N))
item_count=${#ordered_items[@]}
if (( item_count != 1000 )); then
    print -u2 -- "Expected 1000 visible items in fixture: $case_dir (actual=$item_count)"
    exit 1
fi
initial_path="${ordered_items[1]}"
after_start_path="${ordered_items[2]}"
if [[ "${initial_path:t}" != item-00000.txt ]]; then
    print -u2 -- "Unexpected first item in name order: $initial_path"
    exit 1
fi
if [[ "$preflight_only" == 1 ]]; then
    print -- "Hold benchmark preflight passed ($content_profile, $item_count items)."
    exit 0
fi

monotonic_seconds() {
    /usr/bin/perl -MTime::HiRes=clock_gettime,CLOCK_MONOTONIC \
        -e 'printf "%.9f\n", clock_gettime(CLOCK_MONOTONIC)'
}

mkdir -p "$result_root"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
outcomes_file="$result_root/hold-navigation-$content_profile-$run_id.outcomes.tsv"
environment_file="$result_root/hold-navigation-$content_profile-$run_id.environment.txt"
print -r -- $'iteration\tresult\trepeat_steps\trepeat_elapsed_ms\teffective_steps_per_second\trepeat_started_after_keydown_ms\tstop_return_ms\treported_position\tfinal_position\tposition_offset\tfinal_path_at_return\tfinal_path_25ms\tfinal_path_50ms\tfinal_path_100ms\tpost_return_drift' > "$outcomes_file"

window_id=""
stopper_pid=""
stop_timestamp_file=""

close_test_window() {
    if [[ -z "$window_id" ]]; then
        return
    fi
    typeset -a close_script=(
        -e 'set windowId to (system attribute "FINDER_VIM_WINDOW_ID") as integer'
        -e 'tell application "Finder"'
        -e 'if exists (first Finder window whose id is windowId) then close (first Finder window whose id is windowId)'
        -e 'end tell'
    )
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript "${close_script[@]}" \
        >/dev/null 2>&1 || true
    window_id=""
}

cleanup() {
    if [[ -n "$stopper_pid" ]]; then
        wait "$stopper_pid" 2>/dev/null || true
        stopper_pid=""
    fi
    close_test_window
}

activate_test_window() {
    typeset -a activate_script=(
        -e 'set windowId to (system attribute "FINDER_VIM_WINDOW_ID") as integer'
        -e 'tell application "Finder"'
        -e 'set testWindow to first Finder window whose id is windowId'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'end tell'
    )
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript "${activate_script[@]}" \
        >/dev/null 2>&1
}

selected_path() {
    local path
    activate_test_window
    path="$(/usr/bin/osascript \
        -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' \
        2>/dev/null || true)"
    if [[ "$path" != / ]]; then
        path="${path%/}"
    fi
    print -r -- "$path"
}

wait_for_selected_path() {
    local expected="$1"
    local actual
    for ((attempt = 1; attempt <= 60; ++attempt)); do
        actual="$(selected_path)"
        if [[ "$actual" == "$expected" ]]; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

open_test_window() {
    typeset -a open_script=(
        -e 'on run argv'
        -e 'set casePath to item 1 of argv'
        -e 'set initialPath to item 2 of argv'
        -e 'tell application "Finder"'
        -e 'set targetFolder to POSIX file casePath as alias'
        -e 'set initialItem to POSIX file initialPath as alias'
        -e 'set testWindow to make new Finder window'
        -e 'set target of testWindow to targetFolder'
        -e 'set current view of testWindow to list view'
        -e 'set sort column of list view options of testWindow to name column'
        -e 'set selection to {initialItem}'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'return id of testWindow'
        -e 'end tell'
        -e 'end run'
    )
    window_id="$(/usr/bin/osascript "${open_script[@]}" -- \
        "$case_dir" "$initial_path")"
}

trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

commit="$(git -C "$repo_root" rev-parse HEAD)"
dirty=false
if [[ -n "$(git -C "$repo_root" status --porcelain)" ]]; then
    dirty=true
fi
finder_version="$(defaults read /System/Library/CoreServices/Finder.app/Contents/Info CFBundleShortVersionString 2>/dev/null || print unknown)"
key_repeat="$(defaults read -g KeyRepeat 2>/dev/null || print default)"
initial_key_repeat="$(defaults read -g InitialKeyRepeat 2>/dev/null || print default)"
hardware_model="$(sysctl -n hw.model 2>/dev/null || print unknown)"
{
    print -- "run_id=$run_id"
    print -- "commit=$commit"
    print -- "dirty=$dirty"
    print -- "macos_version=$(sw_vers -productVersion)"
    print -- "macos_build=$(sw_vers -buildVersion)"
    print -- "finder_version=$finder_version"
    print -- "hardware_model=$hardware_model"
    print -- "architecture=$(uname -m)"
    print -- "view=list"
    print -- "sort_order=name"
    print -- "grouping=not-controlled"
    print -- "content_profile=$content_profile"
    print -- "item_count=$item_count"
    print -- "iterations=$iterations"
    print -- "hold_duration_ms=$hold_duration_ms"
    print -- "held_threshold_ms=$held_threshold_ms"
    print -- "post_return_observation_ms=0 25 50 100"
    print -- "global_key_repeat=$key_repeat"
    print -- "global_initial_key_repeat=$initial_key_repeat"
    print -- "helper=$helper"
    print -- "hold_token_file=$token_file"
} > "$environment_file"

hold_seconds="$(( hold_duration_ms / 1000.0 ))"

for ((iteration = 1; iteration <= iterations; ++iteration)); do
    open_test_window
    sleep 0.5
    activate_test_window
    "$helper" first >/dev/null
    if ! wait_for_selected_path "$initial_path"; then
        print -u2 -- "Finder context did not become ready: iteration $iteration"
        exit 1
    fi

    keydown_started="$(monotonic_seconds)"
    "$helper" hold-start down
    if ! wait_for_selected_path "$after_start_path"; then
        print -u2 -- "Initial held movement did not complete: iteration $iteration"
        exit 1
    fi
    now="$(monotonic_seconds)"
    typeset -F 6 remaining_threshold
    remaining_threshold=$((
        held_threshold_ms / 1000.0 - (now - keydown_started)
    ))
    if (( remaining_threshold < 0 )); then
        remaining_threshold=0
    fi
    sleep "$remaining_threshold"

    stop_timestamp_file="$result_root/.hold-stop-$run_id-$iteration"
    (
        sleep "$hold_seconds"
        monotonic_seconds > "$stop_timestamp_file"
        truncate -s 0 "$token_file"
    ) &
    stopper_pid=$!

    repeat_started="$(monotonic_seconds)"
    repeat_position="$("$helper" hold-repeat down)"
    repeat_finished="$(monotonic_seconds)"
    wait "$stopper_pid"
    stopper_pid=""
    stop_requested="$(<"$stop_timestamp_file")"
    rm -f "$stop_timestamp_file"

    result=pass
    if [[ ! "$repeat_position" =~ '^[1-9][0-9]*$' ]]; then
        result=fail
        repeat_position=0
    fi
    final_path_at_return="$(selected_path)"
    sleep 0.025
    final_path_25ms="$(selected_path)"
    sleep 0.025
    final_path_50ms="$(selected_path)"
    sleep 0.05
    final_path_100ms="$(selected_path)"
    post_return_drift=no
    if [[ "$final_path_25ms" != "$final_path_at_return"
        || "$final_path_50ms" != "$final_path_at_return"
        || "$final_path_100ms" != "$final_path_at_return" ]]; then
        post_return_drift=yes
        result=fail
    fi

    final_position=0
    for ((item_position = 1; item_position <= item_count; ++item_position)); do
        if [[ "${ordered_items[$item_position]}" == "$final_path_at_return" ]]; then
            final_position=$item_position
            break
        fi
    done
    repeat_steps=$(( final_position > 2 ? final_position - 2 : 0 ))
    if (( final_position <= 2 )); then
        result=fail
    fi
    position_offset=$((
        repeat_position > 0 && final_position > 0
            ? repeat_position - final_position
            : 0
    ))

    calculations="$(awk -v steps="$repeat_steps" -v started="$repeat_started" \
        -v finished="$repeat_finished" -v keydown="$keydown_started" \
        -v stopped="$stop_requested" 'BEGIN {
            elapsed_ms = (finished - started) * 1000
            rate = elapsed_ms > 0 ? steps * 1000 / elapsed_ms : 0
            start_delay_ms = (started - keydown) * 1000
            stop_return_ms = (finished - stopped) * 1000
            printf "%.3f\t%.3f\t%.3f\t%.3f", elapsed_ms, rate,
                start_delay_ms, stop_return_ms
        }')"
    print -r -- "$iteration"$'\t'"$result"$'\t'"$repeat_steps"$'\t'"$calculations"$'\t'"$repeat_position"$'\t'"$final_position"$'\t'"$position_offset"$'\t'"$final_path_at_return"$'\t'"$final_path_25ms"$'\t'"$final_path_50ms"$'\t'"$final_path_100ms"$'\t'"$post_return_drift" >> "$outcomes_file"
    close_test_window
    stop_timestamp_file=""
done

print -- "Outcomes: $outcomes_file"
print -- "Environment: $environment_file"
awk -F '\t' '
NR == 1 { next }
{
    samples++
    rates[samples] = $5 + 0
    stops[samples] = $7 + 0
    steps += $3
    if ($2 != "pass") failures++
}
END {
    for (i = 2; i <= samples; ++i) {
        value = rates[i]
        j = i - 1
        while (j >= 1 && rates[j] > value) {
            rates[j + 1] = rates[j]
            --j
        }
        rates[j + 1] = value

        value = stops[i]
        j = i - 1
        while (j >= 1 && stops[j] > value) {
            stops[j + 1] = stops[j]
            --j
        }
        stops[j + 1] = value
    }
    p50 = int((samples * 50 + 99) / 100)
    p95 = int((samples * 95 + 99) / 100)
    print "samples\ttotal_steps\trate_p50_steps_per_second\trate_p95_steps_per_second\tstop_p50_ms\tstop_p95_ms\tfailures"
    printf "%d\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%d\n",
        samples, steps, rates[p50], rates[p95], stops[p50], stops[p95],
        failures + 0
    exit failures > 0
}
' "$outcomes_file"
