#!/bin/zsh
set -euo pipefail

metrics_file="${1:-}"
if [[ -z "$metrics_file" || ! -r "$metrics_file" ]]; then
    print -u2 -- "Usage: $0 METRICS.tsv"
    exit 64
fi

awk -F '\t' '
NR == 1 {
    if ($1 != "timestamp_ns" || $18 != "dropped_records") {
        print "Unexpected metrics header" > "/dev/stderr"
        exit 65
    }
    next
}
{
    group = $2 SUBSEP $4 SUBSEP $5
    count[group]++
    dispatch[group SUBSEP count[group]] = $6 + 0
    worker[group SUBSEP count[group]] = $7 + 0
    cpu[group] += ($8 + 0) + ($9 + 0)
    wakeups[group] += ($10 + 0) + ($11 + 0)
    ax_reads[group] += $14 + 0
    ax_writes[group] += $15 + 0
    if (($13 + 0) > max_footprint[group]) max_footprint[group] = $13 + 0
    if (($17 + 0) <= 0) failures[group]++
    if (($18 + 0) > dropped[group]) dropped[group] = $18 + 0
    process_key = group SUBSEP $3
    if (!seen_process[process_key]++) processes[group]++
}
END {
    print "label\tworker_state\tcommand\tsamples\tdispatch_p50_ms\tdispatch_p95_ms\tdispatch_p99_ms\tdispatch_max_ms\tworker_p95_ms\tavg_cpu_ms\tavg_wakeups\tavg_ax_reads\tavg_ax_writes\tmax_footprint_bytes\tprocesses\tfailures\tdropped"
    for (group in count) {
        n = count[group]
        for (i = 1; i <= n; ++i) {
            sorted_dispatch[i] = dispatch[group SUBSEP i]
            sorted_worker[i] = worker[group SUBSEP i]
        }
        for (i = 2; i <= n; ++i) {
            value = sorted_dispatch[i]
            j = i - 1
            while (j >= 1 && sorted_dispatch[j] > value) {
                sorted_dispatch[j + 1] = sorted_dispatch[j]
                --j
            }
            sorted_dispatch[j + 1] = value

            value = sorted_worker[i]
            j = i - 1
            while (j >= 1 && sorted_worker[j] > value) {
                sorted_worker[j + 1] = sorted_worker[j]
                --j
            }
            sorted_worker[j + 1] = value
        }
        p50 = int((n * 50 + 99) / 100)
        p95 = int((n * 95 + 99) / 100)
        p99 = int((n * 99 + 99) / 100)
        split(group, parts, SUBSEP)
        printf "%s\t%s\t%s\t%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.3f\t%.3f\t%.3f\t%.0f\t%d\t%d\t%d\n",
            parts[1],
            parts[2],
            parts[3],
            n,
            sorted_dispatch[p50] / 1000000,
            sorted_dispatch[p95] / 1000000,
            sorted_dispatch[p99] / 1000000,
            sorted_dispatch[n] / 1000000,
            sorted_worker[p95] / 1000000,
            cpu[group] / n / 1000000,
            wakeups[group] / n,
            ax_reads[group] / n,
            ax_writes[group] / n,
            max_footprint[group],
            processes[group],
            failures[group] + 0,
            dropped[group] + 0
    }
}
' "$metrics_file"
