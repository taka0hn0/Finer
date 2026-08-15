static CFIndex worker_move(
    navigation_context_t *context,
    bool *context_created,
    direction_t direction,
    int movement_lock_fd,
    bool use_navigation_anchor,
    column_phase_metrics_t *column_phases
) {
    CFIndex validated_index = current_index_unknown;
    if (*context_created) {
        uint64_t validation_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        validated_index = current_index(context);
        if (column_phases) {
            column_phases->context_validation_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                validation_started_nanoseconds
            );
        }
    }
    if (*context_created && validated_index < 0) {
        navigation_context_release(context);
        *context_created = false;
    }
    if (!*context_created) {
        uint64_t creation_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        *context_created = navigation_context_create(context);
        if (column_phases) {
            column_phases->context_creation_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                creation_started_nanoseconds
            );
        }
        if (!*context_created) return false;
        validated_index = current_index(context);
    }

    CFIndex position = move_worker_context(
        context,
        context_created,
        direction,
        validated_index,
        movement_lock_fd,
        use_navigation_anchor,
        column_phases
    );
    if (position > 0) return position;

    if (*context_created) navigation_context_release(context);
    uint64_t creation_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    *context_created = navigation_context_create(context);
    if (column_phases) {
        column_phases->context_creation_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            creation_started_nanoseconds
        );
    }
    if (!*context_created) return 0;
    validated_index = current_index(context);
    return move_worker_context(
        context,
        context_created,
        direction,
        validated_index,
        movement_lock_fd,
        false,
        column_phases
    );
}

static CFIndex execute_worker_command(
    navigation_context_t *context,
    bool *context_created,
    char command,
    int movement_lock_fd,
    bool cold,
    uint64_t submitted_nanoseconds
) {
    if (command == 'e') {
        metrics_snapshot_t start;
        if (metrics_path) start = capture_metrics_snapshot();
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_EX);
        bool success = clear_focused_selection_fallback();
        if (!success && !*context_created) {
            *context_created = navigation_context_create(context);
        }
        if (!success && *context_created) {
            success = clear_selection(context);
        }
        if (*context_created) {
            navigation_context_release(context);
            *context_created = false;
        }
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_UN);
        if (metrics_path) {
            metrics_snapshot_t end = capture_metrics_snapshot();
            record_command_metrics(
                command,
                cold,
                submitted_nanoseconds,
                &start,
                &end,
                success ? 1 : 0,
                NULL
            );
        }
        return success ? 1 : 0;
    }

    if (command == 's') {
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_EX);
        if (*context_created) {
            navigation_context_release(context);
            *context_created = false;
        }
        bool success = run_toggle_mark_helper();
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_UN);
        return success ? 1 : 0;
    }

    direction_t direction;
    if (!direction_from_command(command, &direction)) return 0;

    metrics_snapshot_t start;
    if (metrics_path) start = capture_metrics_snapshot();
    column_phase_metrics_t column_phases;
    column_phase_metrics_t *active_column_phases = NULL;
    if (column_phase_metrics_enabled) {
        memset(&column_phases, 0, sizeof(column_phases));
        active_column_phases = &column_phases;
    }
    CFIndex position = worker_move(
        context,
        context_created,
        direction,
        movement_lock_fd,
        true,
        active_column_phases
    );
    if (position > 0 && *context_created
        && context->has_marks
        && !write_navigation_anchor(context)) {
        position = 0;
    }
    if (metrics_path) {
        metrics_snapshot_t end = capture_metrics_snapshot();
        record_command_metrics(
            command,
            cold,
            submitted_nanoseconds,
            &start,
            &end,
            position,
            active_column_phases
        );
    }
    return position;
}

static int run_worker(
    char initial_command,
    int worker_lock_fd,
    uint64_t initial_submitted_nanoseconds
) {
    struct sockaddr_un address;
    if (!socket_address(&address)) {
        if (worker_lock_fd >= 0) close(worker_lock_fd);
        return 1;
    }

    unlink(address.sun_path);
    int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_fd < 0
        || bind(
            socket_fd,
            (const struct sockaddr *)&address,
            sizeof(address)
        ) != 0) {
        if (socket_fd >= 0) close(socket_fd);
        if (worker_lock_fd >= 0) close(worker_lock_fd);
        return 1;
    }
    chmod(address.sun_path, 0600);

    navigation_context_t context;
    memset(&context, 0, sizeof(context));
    bool context_created = false;
    int movement_lock_fd = open_movement_lock();

    CFIndex initial_result = 0;
    if (AXIsProcessTrusted() && finder_is_frontmost(NULL, NULL)) {
        initial_result = execute_worker_command(
            &context,
            &context_created,
            initial_command,
            movement_lock_fd,
            true,
            initial_submitted_nanoseconds
        );
    }
    if (initial_result == 0) {
        if (movement_lock_fd >= 0) close(movement_lock_fd);
        if (context_created) navigation_context_release(&context);
        close(socket_fd);
        unlink(address.sun_path);
        if (worker_lock_fd >= 0) {
            flock(worker_lock_fd, LOCK_UN);
            close(worker_lock_fd);
        }
        flush_metrics();
        return 1;
    }

    struct pollfd poll_descriptor = {
        .fd = socket_fd,
        .events = POLLIN,
        .revents = 0,
    };
    while (poll(&poll_descriptor, 1, worker_idle_timeout_milliseconds) > 0) {
        if (!(poll_descriptor.revents & POLLIN)) break;

        worker_command_t command;
        ssize_t received = recv(socket_fd, &command, sizeof(command), 0);
        if (received != sizeof(command)) continue;
        if (!finder_is_frontmost(NULL, NULL)) break;
        CFIndex result = execute_worker_command(
            &context,
            &context_created,
            command.command,
            movement_lock_fd,
            false,
            command.submitted_nanoseconds
        );
        if (result == 0) {
            break;
        }
    }

    if (movement_lock_fd >= 0) close(movement_lock_fd);
    if (context_created) navigation_context_release(&context);
    close(socket_fd);
    unlink(address.sun_path);
    if (worker_lock_fd >= 0) {
        flock(worker_lock_fd, LOCK_UN);
        close(worker_lock_fd);
    }
    flush_metrics();
    return 0;
}
