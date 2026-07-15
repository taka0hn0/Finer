#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
result_root="${FINDER_VIM_RESULT_ROOT:-$repo_root/.build/benchmark-results}"
helper="${FINDER_VIM_HELPER:-$HOME/.local/libexec/finder-vim/finder_ax_step}"
iterations="${1:-10}"
timeouts_string="${FINDER_VIM_BENCHMARK_TIMEOUTS:-300 500 750 1000 1500}"
gaps_string="${FINDER_VIM_BENCHMARK_GAPS:-100 400 700 1100 1600}"
timeouts=("${(@s: :)timeouts_string}")
gaps=("${(@s: :)gaps_string}")

if [[ ! "$iterations" =~ '^[1-9][0-9]*$' ]]; then
    print -u2 -- "Iterations must be a positive integer: $iterations"
    exit 64
fi
for timeout in "${timeouts[@]}"; do
    if [[ "$timeout" != 300 && "$timeout" != 500 && "$timeout" != 750 \
        && "$timeout" != 1000 && "$timeout" != 1500 ]]; then
        print -u2 -- "Unsupported worker idle timeout: $timeout"
        exit 64
    fi
done
for gap in "${gaps[@]}"; do
    if [[ ! "$gap" =~ '^[1-9][0-9]*$' ]]; then
        print -u2 -- "Tap gap must be a positive integer: $gap"
        exit 64
    fi
done
if [[ ! -x "$helper" ]]; then
    print -u2 -- "Missing executable helper: $helper"
    print -u2 -- "Run make install or set FINDER_VIM_HELPER."
    exit 1
fi
if [[ ! -f "$fixture_root/items-10/01-A/item-00000.txt" \
    || ! -f "$fixture_root/items-10/01-A/item-00002.txt" ]]; then
    print -u2 -- "Missing empty-files fixture: $fixture_root/items-10/01-A"
    print -u2 -- "Run make benchmark-fixtures COUNTS=10."
    exit 1
fi
if [[ ! -r "$fixture_root/.content-profile" \
    || "$(<"$fixture_root/.content-profile")" != empty-files ]]; then
    print -u2 -- "Worker timeout benchmark requires the empty-files profile."
    exit 1
fi

mkdir -p "$result_root"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
metrics_file="$result_root/worker-idle-timeout-$run_id.metrics.tsv"
outcomes_file="$result_root/worker-idle-timeout-$run_id.outcomes.tsv"
environment_file="$result_root/worker-idle-timeout-$run_id.environment.txt"
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

selected_path() {
    activate_test_window
    /usr/bin/osascript \
        -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)' \
        2>/dev/null || true
}

wait_for_selected_path() {
    local expected="$1"
    local actual
    for ((attempt = 1; attempt <= 60; ++attempt)); do
        actual="$(selected_path)"
        if [[ "$actual" == "$expected" ]]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

open_test_window() {
    local case_dir="$fixture_root/items-10/01-A"
    typeset -a open_script=(
        -e 'set casePath to system attribute "FINDER_VIM_CASE_DIR"'
        -e 'tell application "Finder"'
        -e 'set targetFolder to POSIX file casePath as alias'
        -e 'set testWindow to make new Finder window to targetFolder'
        -e 'set current view of testWindow to list view'
        -e 'set sort column of list view options of testWindow to name column'
        -e 'set selection to {file "item-00000.txt" of targetFolder}'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'return id of testWindow'
        -e 'end tell'
    )
    window_id="$(FINDER_VIM_CASE_DIR="$case_dir" /usr/bin/osascript "${open_script[@]}")"
}

reset_test_selection() {
    typeset -a reset_script=(
        -e 'set windowId to (system attribute "FINDER_VIM_WINDOW_ID") as integer'
        -e 'tell application "Finder"'
        -e 'set testWindow to first Finder window whose id is windowId'
        -e 'set targetFolder to target of testWindow as alias'
        -e 'set selection to {file "item-00000.txt" of targetFolder}'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'end tell'
    )
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript "${reset_script[@]}" \
        >/dev/null
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
helper_sha256="$(shasum -a 256 "$helper" | awk '{ print $1 }')"
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
    print -- "view=list"
    print -- "sort_order=name"
    print -- "grouping=not-controlled"
    print -- "preview_column=not-applicable"
    print -- "content_profile=empty-files"
    print -- "item_count=10"
    print -- "iterations=$iterations"
    print -- "timeouts_ms=$timeouts_string"
    print -- "tap_gaps_ms=$gaps_string"
    print -- "helper=$helper"
    print -- "helper_sha256=$helper_sha256"
} > "$environment_file"

print -r -- $'timeout_ms\tgap_ms\titeration\tresult\tprocesses\tcold_records\twarm_records\tavg_process_exit_ms\tselected_path' > "$outcomes_file"
expected_metrics_rows=0
initial_path="$fixture_root/items-10/01-A/item-00000.txt"
expected_path="$fixture_root/items-10/01-A/item-00002.txt"

for timeout in "${timeouts[@]}"; do
    for gap in "${gaps[@]}"; do
        label="idle-${timeout}-gap-${gap}"
        open_test_window
        for ((iteration = 1; iteration <= iterations; ++iteration)); do
            reset_test_selection
            sleep 0.2
            activate_test_window
            "$helper" first >/dev/null
            if ! wait_for_selected_path "$initial_path"; then
                print -u2 -- "Finder context did not become ready: $label iteration $iteration"
                exit 1
            fi

            FINDER_VIM_METRICS_FILE="$metrics_file" \
            FINDER_VIM_METRICS_LABEL="$label" \
            FINDER_VIM_BENCHMARK_IDLE_TIMEOUT_MS="$timeout" \
                "$helper" hold-start down
            sleep "$(( gap / 1000.0 ))"
            FINDER_VIM_METRICS_FILE="$metrics_file" \
            FINDER_VIM_METRICS_LABEL="$label" \
            FINDER_VIM_BENCHMARK_IDLE_TIMEOUT_MS="$timeout" \
                "$helper" hold-start down

            result=pass
            final_path=""
            if wait_for_selected_path "$expected_path"; then
                final_path="$expected_path"
            else
                result=fail
                final_path="$(selected_path)"
            fi

            (( expected_metrics_rows += 2 ))
            wait_for_metrics_flush "$expected_metrics_rows"
            if [[ "$(awk -F '\t' 'NR == 1 { print $19 }' "$metrics_file")" \
                != worker_exit_after_command_ns ]]; then
                print -u2 -- "Installed helper does not expose worker exit metrics."
                print -u2 -- "Run make install and retry."
                exit 1
            fi

            pair_summary="$(tail -n 2 "$metrics_file" | awk -F '\t' '
                {
                    processes[$3] = 1
                    if ($4 == "cold") cold++
                    if ($4 == "warm") warm++
                    last_exit[$3] = $19 + 0
                }
                END {
                    for (pid in processes) {
                        process_count++
                        exit_total += last_exit[pid]
                    }
                    average_exit_ms = 0
                    if (process_count > 0) {
                        average_exit_ms = exit_total / process_count / 1000000
                    }
                    printf "%d\t%d\t%d\t%.6f", process_count, cold, warm,
                        average_exit_ms
                }
            ')"
            print -r -- "$timeout"$'\t'"$gap"$'\t'"$iteration"$'\t'"$result"$'\t'"$pair_summary"$'\t'"$final_path" >> "$outcomes_file"
        done
        close_test_window
    done
done

print -- "Metrics: $metrics_file"
print -- "Outcomes: $outcomes_file"
print -- "Environment: $environment_file"
awk -F '\t' '
NR == 1 { next }
{
    key = $1 SUBSEP $2
    samples[key]++
    if ($5 == 1) reused[key]++
    processes[key] += $5
    exit_ms[key] += $8
    if ($4 != "pass") failures[key]++
}
END {
    print "timeout_ms\tgap_ms\tsamples\treuse_percent\tavg_processes\tavg_process_exit_ms\tfailures"
    for (key in samples) {
        split(key, parts, SUBSEP)
        printf "%d\t%d\t%d\t%.1f\t%.3f\t%.3f\t%d\n",
            parts[1], parts[2], samples[key],
            reused[key] * 100 / samples[key],
            processes[key] / samples[key],
            exit_ms[key] / samples[key],
            failures[key] + 0
    }
}
' "$outcomes_file"

awk -F '\t' '
NR > 1 && ($4 != "pass" || $6 < 1 || $6 > 2 || $7 < 0 || $7 > 1) {
    print "Invalid timeout outcome: " $0 > "/dev/stderr"
    failed = 1
}
END { exit failed }
' "$outcomes_file"
