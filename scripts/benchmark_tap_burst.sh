#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
result_root="${FINDER_VIM_RESULT_ROOT:-$repo_root/.build/benchmark-results}"
helper="${FINDER_VIM_HELPER:-$HOME/.local/libexec/finder-vim/finder_ax_step}"
state_root="${FINDER_VIM_STATE_ROOT:-$HOME/.local/state/finder-vim}"
content_profile="${FINDER_VIM_BENCHMARK_PROFILE:-empty-files}"
preflight_only="${FINDER_VIM_BENCHMARK_PREFLIGHT:-0}"
interval_ms="${FINDER_VIM_TAP_INTERVAL_MS:-100}"
settle_ms="${FINDER_VIM_TAP_SETTLE_MS:-1000}"
iterations="${1:-10}"
case_root="$fixture_root/items-1000"
child_dir="$case_root/01-A"

if [[ ! "$iterations" =~ '^[1-9][0-9]*$' ]]; then
    print -u2 -- "Iterations must be a positive integer: $iterations"
    exit 64
fi
if [[ ! "$interval_ms" =~ '^[1-9][0-9]*$'
    || "$interval_ms" -lt 20 || "$interval_ms" -gt 1000 ]]; then
    print -u2 -- "Tap interval must be between 20 and 1000ms: $interval_ms"
    exit 64
fi
if [[ ! "$settle_ms" =~ '^[1-9][0-9]*$'
    || "$settle_ms" -lt 100 || "$settle_ms" -gt 5000 ]]; then
    print -u2 -- "Settle time must be between 100 and 5000ms: $settle_ms"
    exit 64
fi
if [[ "$preflight_only" != 0 && "$preflight_only" != 1 ]]; then
    print -u2 -- "FINDER_VIM_BENCHMARK_PREFLIGHT must be 0 or 1: $preflight_only"
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
if [[ ! -r "$fixture_root/.content-profile"
    || "$(<"$fixture_root/.content-profile")" != "$content_profile" ]]; then
    print -u2 -- "Fixture profile does not match $content_profile: $fixture_root"
    print -u2 -- "Run the matching benchmark fixture target."
    exit 1
fi

typeset -a child_items=("$child_dir"/*(N))
if (( ${#child_items[@]} != 1000 )); then
    print -u2 -- \
        "Expected 1000 visible child items: $child_dir (actual=${#child_items[@]})"
    exit 1
fi
if [[ "${child_items[1]:t}" != item-00000.txt
    || ! -f "$case_root/00-start.txt"
    || ! -d "$case_root/01-A"
    || ! -f "$case_root/02-sibling.txt" ]]; then
    print -u2 -- "Unexpected 1000-item fixture layout: $case_root"
    exit 1
fi

typeset -a scenarios=(list_jk icon_hl column_hjkl)
typeset -A views case_paths initial_paths expected_paths
typeset -A direction_strings key_strings

views[list_jk]=list
case_paths[list_jk]="$child_dir"
initial_paths[list_jk]="${child_items[1]}"
expected_paths[list_jk]="${child_items[8]}"
direction_strings[list_jk]='down down down down down down down down down down up up up'
key_strings[list_jk]='j j j j j j j j j j k k k'

views[icon_hl]=icon
case_paths[icon_hl]="$child_dir"
initial_paths[icon_hl]="${child_items[1]}"
expected_paths[icon_hl]="${child_items[8]}"
direction_strings[icon_hl]='right right right right right right right right right right left left left'
key_strings[icon_hl]='l l l l l l l l l l h h h'

views[column_hjkl]=column
case_paths[column_hjkl]="$case_root"
initial_paths[column_hjkl]="$case_root/00-start.txt"
expected_paths[column_hjkl]="$case_root/01-A"
direction_strings[column_hjkl]='down right down left up down right down left up down right down left up down'
key_strings[column_hjkl]='j l j h k j l j h k j l j h k j'

for direction in down up left right; do
    token_file="$state_root/finder_${direction}_hold.txt"
    if [[ ! -f "$token_file" ]]; then
        print -u2 -- "Missing hold token: $token_file"
        exit 1
    fi
done

typeset -A scenario_steps
for scenario in "$scenarios[@]"; do
    typeset -a checked_directions=(
        "${(s: :)direction_strings[$scenario]}"
    )
    typeset -a checked_keys=("${(s: :)key_strings[$scenario]}")
    if (( ${#checked_directions[@]} != ${#checked_keys[@]} )); then
        print -u2 -- "Direction/key sequence length mismatch: $scenario"
        exit 1
    fi
    scenario_steps[$scenario]=${#checked_directions[@]}
done

if [[ "$preflight_only" == 1 ]]; then
    print -- "Tap burst preflight passed ($content_profile, interval=${interval_ms}ms, "\
"list_jk=${scenario_steps[list_jk]}, icon_hl=${scenario_steps[icon_hl]}, "\
"column_hjkl=${scenario_steps[column_hjkl]})."
    exit 0
fi

mkdir -p "$result_root"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
samples_file="$result_root/tap-burst-$content_profile-$run_id.samples.tsv"
outcomes_file="$result_root/tap-burst-$content_profile-$run_id.outcomes.tsv"
environment_file="$result_root/tap-burst-$content_profile-$run_id.environment.txt"
summary_file="$result_root/tap-burst-$content_profile-$run_id.summary.tsv"

print -r -- $'scenario\titeration\tstep\tkey\tdirection\ttarget_s\tsubmitted_s\tfinished_s\tclient_status' \
    > "$samples_file"
print -r -- $'scenario\titeration\tresult\texpected_path\tfinal_path' \
    > "$outcomes_file"

helper_sha256="$(shasum -a 256 "$helper" | awk '{ print $1 }')"
commit="$(git -C "$repo_root" rev-parse HEAD)"
dirty=false
if [[ -n "$(git -C "$repo_root" status --porcelain)" ]]; then
    dirty=true
fi
finder_version="$(
    defaults read /System/Library/CoreServices/Finder.app/Contents/Info \
        CFBundleShortVersionString 2>/dev/null || print unknown
)"
karabiner_cli="/Library/Application Support/org.pqrs/Karabiner-Elements/bin/karabiner_cli"
karabiner_version=unknown
if [[ -x "$karabiner_cli" ]]; then
    karabiner_version="$("$karabiner_cli" --version 2>/dev/null || print unknown)"
fi
hardware_model="$(sysctl -n hw.model 2>/dev/null || print unknown)"
{
    print -- "run_id=$run_id"
    print -- "commit=$commit"
    print -- "dirty=$dirty"
    print -- "macos_version=$(sw_vers -productVersion)"
    print -- "macos_build=$(sw_vers -buildVersion)"
    print -- "finder_version=$finder_version"
    print -- "karabiner_version=$karabiner_version"
    print -- "hardware_model=$hardware_model"
    print -- "architecture=$(uname -m)"
    print -- "content_profile=$content_profile"
    print -- "item_count=1000"
    print -- "iterations=$iterations"
    print -- "tap_interval_ms=$interval_ms"
    print -- "settle_ms=$settle_ms"
    print -- "scenarios=${(j: :)scenarios}"
    print -- "list_jk_keys=${key_strings[list_jk]}"
    print -- "icon_hl_keys=${key_strings[icon_hl]}"
    print -- "column_hjkl_keys=${key_strings[column_hjkl]}"
    print -- "helper=$helper"
    print -- "helper_sha256=$helper_sha256"
    print -- "timing_clock=zsh_EPOCHREALTIME"
    print -- "measurement_scope=client enqueue scheduling and final Finder selection"
} > "$environment_file"

window_id=""

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
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript "$close_script[@]" \
        >/dev/null 2>&1 || true
    window_id=""
}

cleanup() {
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
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript "$activate_script[@]" \
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

open_test_window() {
    local scenario="$1"
    local view_name="${views[$scenario]}"
    local case_path="${case_paths[$scenario]}"
    local initial_path="${initial_paths[$scenario]}"
    typeset -a open_script=(
        -e 'on run argv'
        -e 'set casePath to item 1 of argv'
        -e 'set viewName to item 2 of argv'
        -e 'set initialPath to item 3 of argv'
        -e 'tell application "Finder"'
        -e 'set targetFolder to POSIX file casePath as alias'
        -e 'set initialItem to POSIX file initialPath as alias'
        -e 'set testWindow to make new Finder window'
        -e 'set target of testWindow to targetFolder'
        -e 'if viewName is "list" then'
        -e 'set current view of testWindow to list view'
        -e 'set sort column of list view options of testWindow to name column'
        -e 'else if viewName is "icon" then'
        -e 'set current view of testWindow to icon view'
        -e 'set arrangement of icon view options of testWindow to arranged by name'
        -e 'else'
        -e 'set current view of testWindow to column view'
        -e 'end if'
        -e 'set selection to {initialItem}'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'return id of testWindow'
        -e 'end tell'
        -e 'end run'
    )
    window_id="$(/usr/bin/osascript "$open_script[@]" -- \
        "$case_path" "$view_name" "$initial_path")"
}

wait_for_initial_path() {
    local scenario="$1"
    local expected="${initial_paths[$scenario]}"
    local actual
    for ((attempt = 1; attempt <= 60; ++attempt)); do
        activate_test_window || true
        "$helper" first >/dev/null 2>&1 || true
        actual="$(selected_path)"
        if [[ "$actual" == "$expected" ]]; then
            sleep 0.1
            actual="$(selected_path)"
            if [[ "$actual" == "$expected" ]]; then
                return 0
            fi
        fi
        sleep 0.05
    done
    print -u2 -- "Finder context did not become ready: $scenario"
    return 1
}

trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

typeset -F 6 settle_seconds=$(( settle_ms / 1000.0 ))
sleep 1

for scenario in "$scenarios[@]"; do
    for ((iteration = 1; iteration <= iterations; ++iteration)); do
        open_test_window "$scenario"
        wait_for_initial_path "$scenario"

        command_failures=0
        "$repo_root/scripts/run_tap_schedule.sh" \
            "$helper" "$state_root" "$interval_ms" "$scenario" "$iteration" \
            "${direction_strings[$scenario]}" "${key_strings[$scenario]}" \
            "$samples_file" || command_failures=1

        sleep "$settle_seconds"
        final_path="$(selected_path)"
        expected_path="${expected_paths[$scenario]}"
        result=pass
        if (( command_failures != 0 )) || [[ "$final_path" != "$expected_path" ]]; then
            result=fail
        fi
        print -r -- "$scenario"$'\t'"$iteration"$'\t'"$result"$'\t'"$expected_path"$'\t'"$final_path" \
            >> "$outcomes_file"
        close_test_window
        sleep 1
    done
done

"$repo_root/scripts/summarize_tap_burst.sh" "$samples_file" \
    "${(j: :)scenarios}" > "$summary_file"

print -- "Samples: $samples_file"
print -- "Outcomes: $outcomes_file"
print -- "Environment: $environment_file"
print -- "Summary: $summary_file"
/bin/cat "$summary_file"

awk -F '\t' '
NR > 1 && $3 != "pass" {
    print "Failed outcome: " $0 > "/dev/stderr"
    failed = 1
}
END { exit failed }
' "$outcomes_file"
