static bool valid_metrics_label(const char *label) {
    if (!label || label[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)label;
         *cursor;
         ++cursor) {
        bool valid = (*cursor >= 'a' && *cursor <= 'z')
            || (*cursor >= 'A' && *cursor <= 'Z')
            || (*cursor >= '0' && *cursor <= '9')
            || *cursor == '-'
            || *cursor == '_'
            || *cursor == '.';
        if (!valid) return false;
    }
    return true;
}

static bool supported_benchmark_idle_timeout(long milliseconds) {
    return milliseconds == 300
        || milliseconds == 500
        || milliseconds == 750
        || milliseconds == 1000
        || milliseconds == 1500;
}

static bool configure_metrics(void) {
    const char *path = getenv("FINDER_VIM_METRICS_FILE");
    metrics_path = path && path[0] != '\0' ? path : NULL;

    const char *label = getenv("FINDER_VIM_METRICS_LABEL");
    metrics_label = valid_metrics_label(label) ? label : "-";

    const char *column_phases = getenv("FINDER_VIM_COLUMN_PHASE_METRICS");
    column_phase_metrics_enabled = metrics_path
        && column_phases
        && strcmp(column_phases, "1") == 0;

    const char *timeout = getenv("FINDER_VIM_BENCHMARK_IDLE_TIMEOUT_MS");
    if (!metrics_path || !timeout || timeout[0] == '\0') return true;

    char *end = NULL;
    errno = 0;
    long milliseconds = strtol(timeout, &end, 10);
    if (errno != 0 || !end || *end != '\0'
        || !supported_benchmark_idle_timeout(milliseconds)) {
        fprintf(
            stderr,
            "finder_ax_step: unsupported benchmark idle timeout: %s\n",
            timeout
        );
        return false;
    }
    worker_idle_timeout_milliseconds = (int)milliseconds;
    return true;
}

static metrics_snapshot_t capture_metrics_snapshot(void) {
    metrics_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.monotonic_nanoseconds = monotonic_nanoseconds();
    snapshot.ax_reads = metrics_ax_reads;
    snapshot.ax_writes = metrics_ax_writes;
    snapshot.cg_events = metrics_cg_events;
    snapshot.usage_available = proc_pid_rusage(
        getpid(),
        RUSAGE_INFO_V4,
        (rusage_info_t *)&snapshot.usage
    ) == 0;
    return snapshot;
}

static uint64_t counter_delta(uint64_t end, uint64_t start) {
    return end >= start ? end - start : 0;
}

static void record_command_metrics(
    char command,
    bool cold,
    uint64_t submitted_nanoseconds,
    const metrics_snapshot_t *start,
    const metrics_snapshot_t *end,
    CFIndex result_position,
    const column_phase_metrics_t *column_phases
) {
    if (!metrics_path) return;
    if (metrics_record_count >= sizeof(metrics_records) / sizeof(metrics_records[0])) {
        ++metrics_dropped_records;
        return;
    }

    metrics_record_t *record = &metrics_records[metrics_record_count++];
    memset(record, 0, sizeof(*record));
    record->timestamp_nanoseconds = realtime_nanoseconds();
    record->submitted_nanoseconds = submitted_nanoseconds;
    record->started_nanoseconds = start->monotonic_nanoseconds;
    record->finished_nanoseconds = end->monotonic_nanoseconds;
    record->ax_reads = counter_delta(end->ax_reads, start->ax_reads);
    record->ax_writes = counter_delta(end->ax_writes, start->ax_writes);
    record->cg_events = counter_delta(end->cg_events, start->cg_events);
    if (column_phases) {
        record->column_phases = *column_phases;
        record->column_phase_metrics_enabled = true;
    }
    record->result_position = result_position;
    record->command = command;
    record->cold = cold;

    if (start->usage_available && end->usage_available) {
        record->user_cpu_nanoseconds = counter_delta(
            end->usage.ri_user_time,
            start->usage.ri_user_time
        );
        record->system_cpu_nanoseconds = counter_delta(
            end->usage.ri_system_time,
            start->usage.ri_system_time
        );
        record->package_idle_wakeups = counter_delta(
            end->usage.ri_pkg_idle_wkups,
            start->usage.ri_pkg_idle_wkups
        );
        record->interrupt_wakeups = counter_delta(
            end->usage.ri_interrupt_wkups,
            start->usage.ri_interrupt_wkups
        );
        record->resident_bytes = end->usage.ri_resident_size;
        record->physical_footprint_bytes = end->usage.ri_phys_footprint;
    }
}

static void flush_metrics(void) {
    if (!metrics_path || metrics_record_count == 0) return;

    int descriptor = open(metrics_path, O_CREAT | O_APPEND | O_WRONLY, 0600);
    if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0) {
        if (descriptor >= 0) close(descriptor);
        return;
    }

    struct stat status;
    if (fstat(descriptor, &status) == 0 && status.st_size == 0) {
        dprintf(
            descriptor,
            "timestamp_ns\tlabel\tpid\tworker_state\tcommand\t"
            "dispatch_to_selection_ns\tworker_duration_ns\tuser_cpu_ns\t"
            "system_cpu_ns\tpackage_idle_wakeups\tinterrupt_wakeups\t"
            "resident_bytes\tphysical_footprint_bytes\tax_reads\t"
            "ax_writes\tcg_events\tresult_position\tdropped_records\t"
            "worker_exit_after_command_ns\tcolumn_phase_metrics_enabled\t"
            "column_context_validation_ns\tcolumn_context_creation_ns\t"
            "column_previous_item_count_ns\tcolumn_movement_ns\t"
            "column_event_post_ns\tcolumn_transition_total_ns\t"
            "column_transition_item_count_ns\tcolumn_transition_focus_ns\t"
            "column_transition_candidate_items_ns\t"
            "column_transition_candidate_selection_ns\t"
            "column_transition_sleep_ns\tcolumn_transition_attempts\t"
            "column_transition_reason\n"
        );
    }

    pid_t pid = getpid();
    uint64_t flush_started_nanoseconds = monotonic_nanoseconds();
    for (size_t index = 0; index < metrics_record_count; ++index) {
        const metrics_record_t *record = &metrics_records[index];
        dprintf(
            descriptor,
            "%" PRIu64 "\t%s\t%d\t%s\t%c\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%ld\t%zu\t%" PRIu64 "\t%d"
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%d\n",
            record->timestamp_nanoseconds,
            metrics_label,
            pid,
            record->cold ? "cold" : "warm",
            record->command,
            counter_delta(
                record->finished_nanoseconds,
                record->submitted_nanoseconds
            ),
            counter_delta(
                record->finished_nanoseconds,
                record->started_nanoseconds
            ),
            record->user_cpu_nanoseconds,
            record->system_cpu_nanoseconds,
            record->package_idle_wakeups,
            record->interrupt_wakeups,
            record->resident_bytes,
            record->physical_footprint_bytes,
            record->ax_reads,
            record->ax_writes,
            record->cg_events,
            (long)record->result_position,
            metrics_dropped_records,
            counter_delta(
                flush_started_nanoseconds,
                record->finished_nanoseconds
            ),
            record->column_phase_metrics_enabled ? 1 : 0,
            record->column_phases.context_validation_nanoseconds,
            record->column_phases.context_creation_nanoseconds,
            record->column_phases.previous_item_count_nanoseconds,
            record->column_phases.movement_nanoseconds,
            record->column_phases.event_post_nanoseconds,
            record->column_phases.transition_total_nanoseconds,
            record->column_phases.transition_item_count_nanoseconds,
            record->column_phases.transition_focus_nanoseconds,
            record->column_phases.transition_candidate_items_nanoseconds,
            record->column_phases.transition_candidate_selection_nanoseconds,
            record->column_phases.transition_sleep_nanoseconds,
            record->column_phases.transition_attempts,
            record->column_phases.transition_reason
        );
    }

    flock(descriptor, LOCK_UN);
    close(descriptor);
}
