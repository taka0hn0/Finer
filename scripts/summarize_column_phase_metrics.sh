#!/bin/zsh
set -euo pipefail

metrics_file="${1:-}"
if [[ -z "$metrics_file" || ! -r "$metrics_file" ]]; then
    print -u2 -- "Usage: $0 METRICS.tsv"
    exit 64
fi

awk -F '\t' '
function percentile(metric, group, sample_count, percent,    i, j, value, position, result, key) {
    for (key in ordered) delete ordered[key]
    for (i = 1; i <= sample_count; ++i) {
        ordered[i] = values[metric SUBSEP group SUBSEP i]
    }
    for (i = 2; i <= sample_count; ++i) {
        value = ordered[i]
        j = i - 1
        while (j >= 1 && ordered[j] > value) {
            ordered[j + 1] = ordered[j]
            --j
        }
        ordered[j + 1] = value
    }
    position = int((sample_count * percent + 99) / 100)
    result = ordered[position]
    for (key in ordered) delete ordered[key]
    return result
}
NR == 1 {
    for (column = 1; column <= NF; ++column) columns[$column] = column
    required_columns = "label worker_state command worker_duration_ns column_phase_metrics_enabled column_context_validation_ns column_context_creation_ns column_previous_item_count_ns column_movement_ns column_event_post_ns column_transition_total_ns column_transition_item_count_ns column_transition_focus_ns column_transition_candidate_items_ns column_transition_candidate_selection_ns column_transition_sleep_ns column_transition_attempts column_transition_reason"
    required_count = split(required_columns, required, " ")
    for (required_index = 1; required_index <= required_count; ++required_index) {
        if (!(required[required_index] in columns)) {
            print "Missing metrics column: " required[required_index] > "/dev/stderr"
            header_invalid = 1
        }
    }
    next
}
header_invalid { next }
$(columns["column_phase_metrics_enabled"]) != 1 { next }
{
    group = $(columns["label"]) SUBSEP $(columns["worker_state"]) SUBSEP $(columns["command"])
    sample = ++count[group]
    values["worker" SUBSEP group SUBSEP sample] = $(columns["worker_duration_ns"]) + 0
    values["validation" SUBSEP group SUBSEP sample] = $(columns["column_context_validation_ns"]) + 0
    values["creation" SUBSEP group SUBSEP sample] = $(columns["column_context_creation_ns"]) + 0
    values["previous_count" SUBSEP group SUBSEP sample] = $(columns["column_previous_item_count_ns"]) + 0
    values["movement" SUBSEP group SUBSEP sample] = $(columns["column_movement_ns"]) + 0
    values["event_post" SUBSEP group SUBSEP sample] = $(columns["column_event_post_ns"]) + 0
    values["transition_total" SUBSEP group SUBSEP sample] = $(columns["column_transition_total_ns"]) + 0
    values["transition_count" SUBSEP group SUBSEP sample] = $(columns["column_transition_item_count_ns"]) + 0
    values["transition_focus" SUBSEP group SUBSEP sample] = $(columns["column_transition_focus_ns"]) + 0
    values["candidate_items" SUBSEP group SUBSEP sample] = $(columns["column_transition_candidate_items_ns"]) + 0
    values["candidate_selection" SUBSEP group SUBSEP sample] = $(columns["column_transition_candidate_selection_ns"]) + 0
    values["transition_sleep" SUBSEP group SUBSEP sample] = $(columns["column_transition_sleep_ns"]) + 0
    values["attempts" SUBSEP group SUBSEP sample] = $(columns["column_transition_attempts"]) + 0
    if (($(columns["column_transition_attempts"]) + 0) > max_attempts[group]) {
        max_attempts[group] = $(columns["column_transition_attempts"]) + 0
    }
    reason = $(columns["column_transition_reason"]) + 0
    reason_count[group SUBSEP reason]++
    total_samples++
}
END {
    if (header_invalid) exit 65
    print "label\tworker_state\tcommand\tsamples\tworker_p95_ms\tcontext_validation_p95_ms\tcontext_creation_p95_ms\tprevious_item_count_p95_ms\tmovement_p95_ms\tevent_post_p95_ms\ttransition_total_p50_ms\ttransition_total_p95_ms\ttransition_item_count_p95_ms\ttransition_focus_p95_ms\ttransition_candidate_items_p95_ms\ttransition_candidate_selection_p95_ms\ttransition_sleep_p95_ms\ttransition_attempts_p95\ttransition_attempts_max\treason_item_count\treason_focused_container\treason_timeout"
    for (group in count) {
        sample_count = count[group]
        split(group, parts, SUBSEP)
        printf "%s\t%s\t%s\t%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.0f\t%.0f\t%d\t%d\t%d\n",
            parts[1],
            parts[2],
            parts[3],
            sample_count,
            percentile("worker", group, sample_count, 95) / 1000000,
            percentile("validation", group, sample_count, 95) / 1000000,
            percentile("creation", group, sample_count, 95) / 1000000,
            percentile("previous_count", group, sample_count, 95) / 1000000,
            percentile("movement", group, sample_count, 95) / 1000000,
            percentile("event_post", group, sample_count, 95) / 1000000,
            percentile("transition_total", group, sample_count, 50) / 1000000,
            percentile("transition_total", group, sample_count, 95) / 1000000,
            percentile("transition_count", group, sample_count, 95) / 1000000,
            percentile("transition_focus", group, sample_count, 95) / 1000000,
            percentile("candidate_items", group, sample_count, 95) / 1000000,
            percentile("candidate_selection", group, sample_count, 95) / 1000000,
            percentile("transition_sleep", group, sample_count, 95) / 1000000,
            percentile("attempts", group, sample_count, 95),
            max_attempts[group] + 0,
            reason_count[group SUBSEP 1] + 0,
            reason_count[group SUBSEP 2] + 0,
            reason_count[group SUBSEP 3] + 0
    }
    if (total_samples == 0) {
        print "No column phase metrics found" > "/dev/stderr"
        exit 66
    }
}
' "$metrics_file"
