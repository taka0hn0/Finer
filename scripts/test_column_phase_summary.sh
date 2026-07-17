#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
temporary_directory="$(
    mktemp -d "${TMPDIR:-/tmp}/finer-column-phase-summary.XXXXXX"
)"
trap 'rm -rf "$temporary_directory"' EXIT

metrics_file="$temporary_directory/metrics.tsv"
print -r -- $'timestamp_ns\tlabel\tpid\tworker_state\tcommand\tdispatch_to_selection_ns\tworker_duration_ns\tuser_cpu_ns\tsystem_cpu_ns\tpackage_idle_wakeups\tinterrupt_wakeups\tresident_bytes\tphysical_footprint_bytes\tax_reads\tax_writes\tcg_events\tresult_position\tdropped_records\tworker_exit_after_command_ns\tcolumn_phase_metrics_enabled\tcolumn_context_validation_ns\tcolumn_context_creation_ns\tcolumn_previous_item_count_ns\tcolumn_movement_ns\tcolumn_event_post_ns\tcolumn_transition_total_ns\tcolumn_transition_item_count_ns\tcolumn_transition_focus_ns\tcolumn_transition_candidate_items_ns\tcolumn_transition_candidate_selection_ns\tcolumn_transition_sleep_ns\tcolumn_transition_attempts\tcolumn_transition_reason' > "$metrics_file"
print -r -- $'1\ttest\t42\twarm\tl\t10000000\t9000000\t0\t0\t0\t0\t1000\t2000\t3\t0\t1\t2\t0\t0\t1\t100000\t200000\t300000\t400000\t500000\t600000\t700000\t800000\t900000\t1000000\t1100000\t2\t2' >> "$metrics_file"
print -r -- $'2\ttest\t42\twarm\tl\t20000000\t19000000\t0\t0\t0\t0\t1000\t3000\t4\t0\t1\t3\t0\t0\t1\t1100000\t1200000\t1300000\t1400000\t1500000\t1600000\t1700000\t1800000\t1900000\t2000000\t2100000\t3\t3' >> "$metrics_file"

generic_summary="$(
    "$repo_root/scripts/summarize_benchmark_metrics.sh" "$metrics_file"
)"
if [[ "$generic_summary" != *$'test\twarm\tl\t2\t'* ]]; then
    print -u2 -- "Generic summary rejected the extended metrics schema"
    exit 1
fi

phase_summary="$(
    "$repo_root/scripts/summarize_column_phase_metrics.sh" "$metrics_file"
)"
phase_line="${phase_summary##*$'\n'}"
expected_line=$'test\twarm\tl\t2\t19.000000\t1.100000\t1.200000\t1.300000\t1.400000\t1.500000\t0.600000\t1.600000\t1.700000\t1.800000\t1.900000\t2.000000\t2.100000\t3\t3\t0\t1\t1'
if [[ "$phase_line" != "$expected_line" ]]; then
    print -u2 -- "Unexpected Column phase summary"
    print -u2 -- "Expected: $expected_line"
    print -u2 -- "Actual:   $phase_line"
    exit 1
fi

print -- "Column phase summary tests passed."
