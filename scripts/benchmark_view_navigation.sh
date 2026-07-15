#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
result_root="${FINDER_VIM_RESULT_ROOT:-$repo_root/.build/benchmark-results}"
helper="${FINDER_VIM_HELPER:-$HOME/.local/libexec/finder-vim/finder_ax_step}"
view="${1:-}"
iterations="${2:-10}"
counts_string="${FINDER_VIM_BENCHMARK_COUNTS:-10 1000 10000}"
counts=("${(@s: :)counts_string}")

case "$view" in
    list)
        directions=(down down down)
        readiness_stability_seconds=0.1
        ;;
    icon)
        directions=(right right right)
        readiness_stability_seconds=1
        ;;
    *)
        print -u2 -- "Usage: $0 {list|icon} [ITERATIONS]"
        exit 64
        ;;
esac
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

mkdir -p "$result_root"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
metrics_file="$result_root/$view-navigation-$run_id.metrics.tsv"
outcomes_file="$result_root/$view-navigation-$run_id.outcomes.tsv"
environment_file="$result_root/$view-navigation-$run_id.environment.txt"
touch "$metrics_file" "$outcomes_file" "$environment_file"
truncate -s 0 "$metrics_file" "$outcomes_file" "$environment_file"

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
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript "${close_script[@]}" \
        >/dev/null 2>&1 || true
    window_id=""
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

wait_for_initial_context() {
    for ((attempt = 1; attempt <= 40; ++attempt)); do
        activate_test_window || true
        if "$helper" first >/dev/null 2>&1; then
            selected_path="$(/usr/bin/osascript -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' 2>/dev/null || true)"
            if [[ "$selected_path" == "$initial_path" ]]; then
                sleep "$readiness_stability_seconds"
                stable_path="$(/usr/bin/osascript -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' 2>/dev/null || true)"
                if [[ "$stable_path" == "$initial_path" ]]; then
                    activate_test_window
                    return 0
                fi
            fi
        fi
        sleep 0.1
    done
    print -u2 -- "Finder context did not become ready: $label iteration $iteration"
    return 1
}

send_benchmark_command() {
    local direction="$1"
    if FINDER_VIM_METRICS_FILE="$metrics_file" FINDER_VIM_METRICS_LABEL="$label" \
        "$helper" hold-start "$direction"; then
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

open_test_window() {
    typeset -a open_script=(
        -e 'set casePath to system attribute "FINDER_VIM_CASE_DIR"'
        -e 'set viewName to system attribute "FINDER_VIM_VIEW"'
        -e 'tell application "Finder"'
        -e 'set targetFolder to POSIX file casePath as alias'
        -e 'set testWindow to make new Finder window to targetFolder'
        -e 'if viewName is "list" then'
        -e 'set current view of testWindow to list view'
        -e 'set sort column of list view options of testWindow to name column'
        -e 'else'
        -e 'set current view of testWindow to icon view'
        -e 'set arrangement of icon view options of testWindow to arranged by name'
        -e 'end if'
        -e 'set selection to {file "item-00000.txt" of targetFolder}'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'return id of testWindow'
        -e 'end tell'
    )
    FINDER_VIM_CASE_DIR="$case_dir" FINDER_VIM_VIEW="$view" \
        /usr/bin/osascript "${open_script[@]}"
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
    print -- "view=$view"
    print -- "sort_order=name"
    print -- "grouping=not-controlled"
    print -- "preview_column=not-applicable"
    print -- "iterations=$iterations"
    print -- "helper=$helper"
} > "$environment_file"

print -r -- $'label\titeration\tresult\tselected_path' > "$outcomes_file"
sleep 1
expected_metrics_rows=0

for count in "${counts[@]}"; do
    label="$view-items-$count"
    case_dir="$fixture_root/items-$count/01-A"
    initial_path="$case_dir/item-00000.txt"
    expected_path="$case_dir/item-00003.txt"
    if [[ ! -f "$initial_path" || ! -f "$expected_path" ]]; then
        print -u2 -- "Missing fixture: $case_dir"
        print -u2 -- "Run make benchmark-fixtures."
        exit 1
    fi

    for ((iteration = 1; iteration <= iterations; ++iteration)); do
        window_id="$(open_test_window)"
        wait_for_initial_context

        for direction in "${directions[@]}"; do
            send_benchmark_command "$direction"
        done

        sleep 1
        activate_test_window
        sleep 0.1
        selected_path="$(/usr/bin/osascript -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' 2>/dev/null || true)"
        result=pass
        if [[ "$selected_path" != "$expected_path" ]]; then
            result=fail
        fi
        print -r -- "$label"$'\t'"$iteration"$'\t'"$result"$'\t'"$selected_path" >> "$outcomes_file"
        close_test_window
        (( expected_metrics_rows += 3 ))
        wait_for_metrics_flush "$expected_metrics_rows"
    done
done

print -- "Metrics: $metrics_file"
print -- "Outcomes: $outcomes_file"
print -- "Environment: $environment_file"
"$repo_root/scripts/summarize_benchmark_metrics.sh" "$metrics_file"

awk -F '\t' -v iterations="$iterations" -v requested_counts="$counts_string" -v view="$view" '
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
        label = view "-items-" requested[label_index]
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
