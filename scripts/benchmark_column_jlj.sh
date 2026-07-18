#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
result_root="${FINDER_VIM_RESULT_ROOT:-$repo_root/.build/benchmark-results}"
helper="${FINDER_VIM_HELPER:-$HOME/.local/libexec/finder-vim/finder_ax_step}"
content_profile="${FINDER_VIM_BENCHMARK_PROFILE:-empty-files}"
iterations="${1:-10}"
counts_string="${FINDER_VIM_BENCHMARK_COUNTS:-10 1000 10000}"
counts=("${(@s: :)counts_string}")

group_menu_item() {
    case "$1" in
        None|Name|Kind|Application|Size|Tags) print "$1" ;;
        DateLastOpened) print 'Date Last Opened' ;;
        DateAdded) print 'Date Added' ;;
        DateModified) print 'Date Modified' ;;
        DateCreated) print 'Date Created' ;;
        *) return 1 ;;
    esac
}

original_group_arrangement="$(defaults read com.apple.finder FXArrangeGroupViewBy 2>/dev/null || print Name)"
original_group_preference="$(defaults read com.apple.finder FXPreferredGroupBy 2>/dev/null || print None)"
original_group_menu_item="$(group_menu_item "$original_group_preference" || true)"
if [[ -z "$original_group_menu_item" ]]; then
    print -u2 -- "Unsupported Finder grouping preference: $original_group_preference"
    exit 1
fi

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
    print -u2 -- "Run make install or set FINDER_VIM_HELPER."
    exit 1
fi
"$repo_root/scripts/require_benchmark_metrics.sh" "$helper"

profile_marker="$fixture_root/.content-profile"
if [[ ! -r "$profile_marker" || "$(<"$profile_marker")" != "$content_profile" ]]; then
    print -u2 -- "Fixture profile does not match $content_profile: $fixture_root"
    print -u2 -- "Run the matching benchmark fixture target."
    exit 1
fi

mkdir -p "$result_root"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
metrics_file="$result_root/column-jlj-$run_id.metrics.tsv"
outcomes_file="$result_root/column-jlj-$run_id.outcomes.tsv"
environment_file="$result_root/column-jlj-$run_id.environment.txt"
touch "$metrics_file" "$outcomes_file" "$environment_file"
truncate -s 0 "$metrics_file" "$outcomes_file" "$environment_file"

window_id=""
grouping_changed=false
close_test_window() {
    local restore="${1:-false}"
    local restore_status=0
    if [[ -z "$window_id" ]]; then
        if [[ "$restore" == true ]]; then
            if ! restore_grouping_preferences; then
                restore_status=1
            fi
        fi
        return "$restore_status"
    fi
    if [[ "$restore" == true ]]; then
        if ! restore_grouping_runtime; then
            print -u2 -- "Failed to restore Finder runtime grouping"
            restore_status=1
        fi
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
    if [[ "$restore" == true ]]; then
        if ! restore_grouping_preferences; then
            print -u2 -- "Failed to restore Finder grouping preferences"
            restore_status=1
        fi
    fi
    return "$restore_status"
}

open_test_window() {
    typeset -a open_script=(
        -e 'set casePath to system attribute "FINDER_VIM_CASE_DIR"'
        -e 'tell application "Finder"'
        -e 'set targetFolder to POSIX file casePath as alias'
        -e 'set testWindow to make new Finder window to targetFolder'
        -e 'set current view of testWindow to column view'
        -e 'set selection to {file "00-start.txt" of targetFolder}'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'return id of testWindow'
        -e 'end tell'
    )
    window_id="$(
        FINDER_VIM_CASE_DIR="$case_dir" /usr/bin/osascript "${open_script[@]}"
    )"
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

set_grouping_criterion() {
    local menu_item="$1"
    activate_test_window
    /usr/bin/osascript \
        -e 'on run argv' \
        -e 'set criterionName to item 1 of argv' \
        -e 'tell application "System Events"' \
        -e 'tell process "Finder"' \
        -e 'set mainWindow to first window whose value of attribute "AXMain" is true' \
        -e 'set groupButton to first menu button of toolbar 1 of mainWindow whose description is "Group"' \
        -e 'click groupButton' \
        -e 'click menu item criterionName of menu 1 of groupButton' \
        -e 'end tell' \
        -e 'end tell' \
        -e 'end run' -- "$menu_item" >/dev/null
    sleep 0.2
}

disable_grouping() {
    if [[ "$grouping_changed" == true ]]; then
        return
    fi
    set_grouping_criterion None
    grouping_changed=true
    # Finder can keep the old grouped AX browser alive after the menu change.
    # Reopen the dedicated window so measurement starts from a fresh tree.
    close_test_window false
    open_test_window
    sleep 1
}

restore_grouping_runtime() {
    if [[ "$grouping_changed" != true || -z "$window_id" ]]; then
        return
    fi
    set_grouping_criterion "$original_group_menu_item"
}

restore_grouping_preferences() {
    if [[ "$grouping_changed" != true ]]; then
        return
    fi
    defaults write com.apple.finder FXArrangeGroupViewBy \
        -string "$original_group_arrangement"
    defaults write com.apple.finder FXPreferredGroupBy \
        -string "$original_group_preference"
    grouping_changed=false
}

initial_context_ready() {
    activate_test_window || return 1
    if "$helper" first >/dev/null 2>&1; then
        selected_path="$(/usr/bin/osascript -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' 2>/dev/null || true)"
        if [[ "$selected_path" == "$initial_path" ]]; then
            sleep 0.1
            stable_path="$(/usr/bin/osascript -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' 2>/dev/null || true)"
            if [[ "$stable_path" == "$initial_path" ]]; then
                activate_test_window
                return 0
            fi
        fi
    fi
    return 1
}

wait_for_initial_context() {
    for ((attempt = 1; attempt <= 20; ++attempt)); do
        if initial_context_ready; then
            return 0
        fi
        sleep 0.1
    done
    print -u2 -- "Finder context did not become ready: $label iteration $iteration"
    return 1
}

prepare_initial_context() {
    # Finder may retain the runtime grouping state after its stored preferences
    # have been restored. Probe the actual new window before sending a shortcut
    # that could otherwise invert an already-ungrouped browser.
    for ((attempt = 1; attempt <= 10; ++attempt)); do
        if initial_context_ready; then
            return 0
        fi
        sleep 0.1
    done
    disable_grouping
    wait_for_initial_context
}

send_benchmark_command() {
    local direction="$1"
    if FINDER_VIM_METRICS_FILE="$metrics_file" FINDER_VIM_METRICS_LABEL="$label" "$helper" hold-start "$direction"; then
        return 0
    fi
    print -u2 -- "Command failed: $label iteration $iteration $direction"
    return 1
}

wait_for_metrics_flush() {
    local expected_rows="$1"
    local rows
    for ((attempt = 1; attempt <= 300; ++attempt)); do
        rows="$(awk 'END { print (NR > 0 ? NR - 1 : 0) }' "$metrics_file")"
        if (( rows >= expected_rows )); then
            return 0
        fi
        sleep 0.1
    done
    print -u2 -- "Metrics did not flush: expected=$expected_rows actual=$rows"
    return 1
}

trap 'close_test_window true' EXIT
trap 'close_test_window true; exit 130' INT
trap 'close_test_window true; exit 143' TERM

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
    print -- "helper=$helper"
    print -- "column_phase_metrics=${FINDER_VIM_COLUMN_PHASE_METRICS:-0}"
    print -- "finder_group_arrangement=$original_group_arrangement"
    print -- "finder_group_preference=$original_group_preference"
    print -- "benchmark_grouping=None"
} > "$environment_file"
if [[ -r "$fixture_root/manifest.tsv" ]]; then
    awk -F '\t' 'NR > 1 {
        printf "fixture_%s_visible_items=%s\n", $2, $3
        printf "fixture_%s_files=%s\n", $2, $4
        printf "fixture_%s_directories=%s\n", $2, $5
        printf "fixture_%s_logical_bytes=%s\n", $2, $6
    }' "$fixture_root/manifest.tsv" >> "$environment_file"
fi

print -r -- $'label\titeration\tresult\tselected_path' > "$outcomes_file"
sleep 1
expected_metrics_rows=0

for count in "${counts[@]}"; do
    label="items-$count"
    case_dir="$fixture_root/$label"
    initial_path="$case_dir/00-start.txt"
    expected_path="$case_dir/01-A/item-00001.txt"
    if [[ ! -f "$case_dir/00-start.txt" || ! -f "$expected_path" ]]; then
        print -u2 -- "Missing fixture: $case_dir"
        print -u2 -- "Run make benchmark-fixtures."
        exit 1
    fi

    for ((iteration = 1; iteration <= iterations; ++iteration)); do
        open_test_window
        prepare_initial_context

        send_benchmark_command down
        send_benchmark_command right
        send_benchmark_command down

        sleep 1
        activate_test_window
        sleep 0.1
        selected_path="$(/usr/bin/osascript -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' 2>/dev/null || true)"
        result=pass
        if [[ "$selected_path" != "$expected_path" ]]; then
            result=fail
        fi
        print -r -- "$label"$'\t'"$iteration"$'\t'"$result"$'\t'"$selected_path" >> "$outcomes_file"
        final_iteration=false
        if [[ "$count" == "${counts[-1]}" && "$iteration" == "$iterations" ]]; then
            final_iteration=true
        fi
        close_test_window "$final_iteration"
        (( expected_metrics_rows += 3 ))
        wait_for_metrics_flush "$expected_metrics_rows"
    done
done

print -- "Metrics: $metrics_file"
print -- "Outcomes: $outcomes_file"
print -- "Environment: $environment_file"
"$repo_root/scripts/summarize_benchmark_metrics.sh" "$metrics_file"
if [[ "${FINDER_VIM_COLUMN_PHASE_METRICS:-0}" == 1 ]]; then
    "$repo_root/scripts/summarize_column_phase_metrics.sh" "$metrics_file"
fi

awk -F '\t' -v iterations="$iterations" -v requested_counts="$counts_string" '
NR == 1 { next }
{
    rows[$2]++
    if ($4 == "cold") cold[$2]++
    if ($4 == "warm") warm[$2]++
    if (($17 + 0) <= 0 || ($18 + 0) != 0) invalid[$2]++
    process[$2 SUBSEP $3] = 1
}
END {
    for (key in process) {
        split(key, parts, SUBSEP)
        processes[parts[1]]++
    }
    label_count = split(requested_counts, requested, " ")
    failed = 0
    for (label_index = 1; label_index <= label_count; ++label_index) {
        label = "items-" requested[label_index]
        if (rows[label] != iterations * 3 || cold[label] != iterations || warm[label] != iterations * 2 || processes[label] != iterations || invalid[label] != 0) {
            printf "Invalid metrics for %s: rows=%d cold=%d warm=%d processes=%d invalid=%d\n",
                label,
                rows[label],
                cold[label],
                warm[label],
                processes[label],
                invalid[label] > "/dev/stderr"
            failed = 1
        }
    }
    exit failed
}
' "$metrics_file"

awk -F '\t' '
NR > 1 && $3 != "pass" {
    print "Failed outcome: " $0 > "/dev/stderr"
    failed = 1
}
END { exit failed }
' "$outcomes_file"
