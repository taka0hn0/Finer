#!/bin/zsh
set -euo pipefail

if (( $# != 2 )); then
    print -u2 -- "Usage: $0 SAMPLES_FILE 'SCENARIO ...'"
    exit 64
fi

samples_file="$1"
scenario_order="$2"
if [[ ! -r "$samples_file" ]]; then
    print -u2 -- "Missing readable samples file: $samples_file"
    exit 1
fi
if [[ -z "$scenario_order" ]]; then
    print -u2 -- "Scenario order must not be empty."
    exit 64
fi

awk -F '\t' -v scenario_order="$scenario_order" '
BEGIN {
    scenario_count = split(scenario_order, ordered, " ")
    for (i = 1; i <= scenario_count; ++i) {
        if (ordered[i] == "" || wanted[ordered[i]]) {
            print "Invalid scenario order: " scenario_order > "/dev/stderr"
            invalid = 1
        }
        wanted[ordered[i]] = 1
    }
}
NR == 1 {
    if (NF != 9 ||
        $1 != "scenario" ||
        $2 != "iteration" ||
        $3 != "step" ||
        $4 != "key" ||
        $5 != "direction" ||
        $6 != "target_s" ||
        $7 != "submitted_s" ||
        $8 != "finished_s" ||
        $9 != "client_status") {
        print "Unexpected tap sample header" > "/dev/stderr"
        invalid = 1
    }
    next
}
{
    if (NF != 9 || !wanted[$1] ||
        $2 !~ /^[1-9][0-9]*$/ ||
        $3 !~ /^[1-9][0-9]*$/ ||
        $6 !~ /^-?[0-9]+([.][0-9]+)?$/ ||
        $7 !~ /^-?[0-9]+([.][0-9]+)?$/ ||
        $8 !~ /^-?[0-9]+([.][0-9]+)?$/ ||
        $9 !~ /^[0-9]+$/) {
        print "Invalid tap sample row " NR ": " $0 > "/dev/stderr"
        invalid = 1
        next
    }
    scenario = $1
    sample = ++samples[scenario]
    duration[scenario SUBSEP sample] = ($8 - $7) * 1000
    lateness[scenario SUBSEP sample] = ($7 - $6) * 1000
    if ($9 != 0) failures[scenario]++
}
END {
    if (NR < 2) {
        print "Tap samples file has no data rows" > "/dev/stderr"
        invalid = 1
    }
    for (scenario_index = 1; scenario_index <= scenario_count; ++scenario_index) {
        scenario = ordered[scenario_index]
        if (samples[scenario] == 0) {
            print "Missing tap samples for scenario: " scenario > "/dev/stderr"
            invalid = 1
        }
    }
    if (invalid) exit 1

    print "scenario\tsamples\tclient_enqueue_p50_ms\tclient_enqueue_p95_ms\tclient_enqueue_max_ms\tsubmit_lateness_p50_ms\tsubmit_lateness_p95_ms\tsubmit_lateness_max_ms\tclient_failures"
    for (scenario_index = 1; scenario_index <= scenario_count; ++scenario_index) {
        scenario = ordered[scenario_index]
        count = samples[scenario]
        for (i = 2; i <= count; ++i) {
            value = duration[scenario SUBSEP i]
            j = i - 1
            while (j >= 1 && duration[scenario SUBSEP j] > value) {
                duration[scenario SUBSEP (j + 1)] = duration[scenario SUBSEP j]
                --j
            }
            duration[scenario SUBSEP (j + 1)] = value

            value = lateness[scenario SUBSEP i]
            j = i - 1
            while (j >= 1 && lateness[scenario SUBSEP j] > value) {
                lateness[scenario SUBSEP (j + 1)] = lateness[scenario SUBSEP j]
                --j
            }
            lateness[scenario SUBSEP (j + 1)] = value
        }
        p50 = int((count * 50 + 99) / 100)
        p95 = int((count * 95 + 99) / 100)
        printf "%s\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%d\n",
            scenario,
            count,
            duration[scenario SUBSEP p50],
            duration[scenario SUBSEP p95],
            duration[scenario SUBSEP count],
            lateness[scenario SUBSEP p50],
            lateness[scenario SUBSEP p95],
            lateness[scenario SUBSEP count],
            failures[scenario] + 0
    }
}
' "$samples_file"
