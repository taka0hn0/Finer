#!/bin/zsh
set -euo pipefail

if [[ "${1:-}" == hold-start ]]; then
    direction="${2:-}"
    if [[ -n "${FINDER_VIM_TAP_TEST_FAIL_DIRECTION:-}"
        && "$direction" == "$FINDER_VIM_TAP_TEST_FAIL_DIRECTION" ]]; then
        exit 23
    fi
    sleep 0.002
    exit 0
fi

repo_root="${0:A:h:h}"
mkdir -p "$repo_root/.build"
test_root="$(mktemp -d "$repo_root/.build/tap-burst-headless.XXXXXX")"
trap 'rm -rf "$test_root"' EXIT

state_root="$test_root/state"
fixture_root="$test_root/fixtures"
samples_file="$test_root/samples.tsv"
summary_file="$test_root/summary.tsv"
mkdir -p "$state_root"
for direction in down up left right; do
    : > "$state_root/finder_${direction}_hold.txt"
done
print -r -- $'scenario\titeration\tstep\tkey\tdirection\ttarget_s\tsubmitted_s\tfinished_s\tclient_status' \
    > "$samples_file"

for interval_ms in 50 100 150; do
    scenario="tap_${interval_ms}"
    "$repo_root/scripts/run_tap_schedule.sh" \
        "$0" "$state_root" "$interval_ms" "$scenario" 1 \
        'down up down up' 'j k j k' "$samples_file"
    awk -F '\t' -v scenario="$scenario" -v interval="$interval_ms" '
    $1 == scenario {
        ++count
        if ($9 != 0 || $7 + 0.001 < $6 || (last_submitted && $7 < last_submitted)) {
            invalid = 1
        }
        if (last_target) {
            delta = ($6 - last_target) * 1000
            if (delta < interval - 0.01 || delta > interval + 0.01) invalid = 1
        }
        last_target = $6
        last_submitted = $7
    }
    END { exit count != 4 || invalid }
    ' "$samples_file"
done

if FINDER_VIM_TAP_TEST_FAIL_DIRECTION=left \
    "$repo_root/scripts/run_tap_schedule.sh" \
        "$0" "$state_root" 100 tap_failure 1 \
        'right left' 'l h' "$samples_file"; then
    print -u2 -- "Expected the failing stub direction to fail the schedule."
    exit 1
fi
awk -F '\t' '
$1 == "tap_failure" { ++count; if ($5 == "left" && $9 == 23) saw_failure = 1 }
END { exit count != 2 || !saw_failure }
' "$samples_file"

print -r -- $'percentiles\t1\t1\tj\tdown\t100.000\t100.010\t100.011\t0' \
    >> "$samples_file"
print -r -- $'percentiles\t1\t2\tj\tdown\t100.000\t100.020\t100.022\t0' \
    >> "$samples_file"
print -r -- $'percentiles\t1\t3\tj\tdown\t100.000\t100.030\t100.033\t0' \
    >> "$samples_file"
print -r -- $'percentiles\t1\t4\tj\tdown\t100.000\t100.040\t100.044\t0' \
    >> "$samples_file"

"$repo_root/scripts/summarize_tap_burst.sh" "$samples_file" \
    'tap_50 tap_100 tap_150 tap_failure percentiles' > "$summary_file"
awk -F '\t' '
NR == 1 { next }
$1 == "tap_50" || $1 == "tap_100" || $1 == "tap_150" {
    ++success_scenarios
    if ($2 != 4 || $9 != 0) invalid = 1
}
$1 == "tap_failure" {
    saw_failure = 1
    if ($2 != 2 || $9 != 1) invalid = 1
}
$1 == "percentiles" {
    saw_percentiles = 1
    if ($2 != 4 || $3 != "2.000" || $4 != "4.000" || $5 != "4.000" ||
        $6 != "20.000" || $7 != "40.000" || $8 != "40.000" ||
        $9 != 0) invalid = 1
}
END {
    exit success_scenarios != 3 || !saw_failure || !saw_percentiles || invalid
}
' "$summary_file"

invalid_samples="$test_root/invalid.tsv"
print -r -- $'scenario\titeration\tstep\tkey\tdirection\ttarget_s\tsubmitted_s\tfinished_s\tclient_status' \
    > "$invalid_samples"
if "$repo_root/scripts/run_tap_schedule.sh" \
    "$0" "$state_root" 19 invalid_interval 1 'down' 'j' "$invalid_samples" \
    >/dev/null 2>&1; then
    print -u2 -- "Expected an invalid tap interval to fail."
    exit 1
fi
if (( $(wc -l < "$invalid_samples") != 1 )); then
    print -u2 -- "Invalid input unexpectedly appended a tap sample."
    exit 1
fi

FINDER_VIM_BENCHMARK_COUNTS=1000 \
    "$repo_root/scripts/prepare_benchmark_fixtures.sh" "$fixture_root" \
    >/dev/null
result_root="$test_root/results"
FINDER_VIM_FIXTURE_ROOT="$fixture_root" \
FINDER_VIM_RESULT_ROOT="$result_root" \
FINDER_VIM_HELPER="$0" \
FINDER_VIM_STATE_ROOT="$state_root" \
FINDER_VIM_BENCHMARK_PREFLIGHT=1 \
    "$repo_root/scripts/benchmark_tap_burst.sh" 1 >/dev/null
if [[ -e "$result_root" ]]; then
    print -u2 -- "Tap preflight unexpectedly created a result directory."
    exit 1
fi

print -- "Headless tap burst tests passed."
