static int run_lightweight_column_edge_monitor(
    direction_t direction,
    int token_fd,
    const char *initial_token,
    ssize_t initial_token_length,
    int movement_lock_fd
) {
    pid_t initial_finder_pid = 0;
    if (!hold_token_matches(
            token_fd,
            initial_token,
            initial_token_length
        )
        || !finder_is_frontmost(&initial_finder_pid, NULL)
        || !mark_state_is_empty()) {
        return 0;
    }

    // The direct Karabiner Arrow gives the first 100ms native response. Take
    // ownership before asking Finder for AX state: a continuously held Arrow
    // can keep synchronous selected-child reads blocked for more than a
    // second. Complete taps leave a gap for Finder to service the lightweight
    // AX request while preserving a 60Hz movement cadence.
    if (movement_lock_fd >= 0) {
        flock(movement_lock_fd, LOCK_EX);
    }
    bool released = post_key_event(
        arrow_key_code(direction),
        false,
        false
    );
    if (movement_lock_fd >= 0) {
        flock(movement_lock_fd, LOCK_UN);
    }
    if (!released) return 1;

    column_edge_context_t context;
    EDGE_TRACE(
        "context-start\tt=%.6f\tkind=column-stall\n",
        monotonic_seconds()
    );
    bool context_created = column_edge_context_create(&context);
    EDGE_TRACE(
        "context-finish\tt=%.6f\tkind=column-stall\n",
        monotonic_seconds()
    );
    if (!context_created) {
        EDGE_TRACE(
            "exit\tt=%.6f\treason=column-context\n",
            monotonic_seconds()
        );
        return 1;
    }
    if (context.finder_pid != initial_finder_pid
        || !hold_token_matches(
            token_fd,
            initial_token,
            initial_token_length
        )
        || !mark_state_is_empty()) {
        column_edge_context_release(&context);
        return 0;
    }

    double deadline = monotonic_seconds() + 300.0;
    column_selection_stall_state_t stall_state;
    column_selection_stall_state_create(&stall_state);
    EDGE_TRACE(
        "selection-seed-start\tt=%.6f\tkind=column-stall\n",
        monotonic_seconds()
    );
    AXUIElementRef current = copy_selected_column_child(context.container);
    EDGE_TRACE(
        "selection-seed-finish\tt=%.6f\tkind=column-stall\n",
        monotonic_seconds()
    );
    if (current) {
        column_selection_stall_state_observe(&stall_state, current);
        CFRelease(current);
    }
    int result = run_owned_column_tap_repeat(
        &context,
        direction,
        token_fd,
        initial_token,
        initial_token_length,
        &stall_state,
        deadline,
        movement_lock_fd
    );

    column_selection_stall_state_release(&stall_state);
    column_edge_context_release(&context);
    return result;
}

static int run_vertical_edge_monitor_worker(
    edge_monitor_view_t view,
    direction_t direction
) {
    if (direction != direction_down && direction != direction_up) return 64;

    int monitor_lock_fd = open_vertical_edge_monitor_lock(view, direction);
    if (monitor_lock_fd < 0) return 1;
    if (!acquire_vertical_edge_monitor_lock(
            monitor_lock_fd,
            view == edge_monitor_column_view
        )) {
        int lock_error = errno;
        close(monitor_lock_fd);
        return lock_error == EWOULDBLOCK
                || lock_error == EAGAIN
                || lock_error == EINTR
            ? 0
            : 1;
    }

#ifdef FINER_EDGE_TRACE
    open_edge_trace(view, direction);
#endif

    int result = 0;
    int movement_lock_fd = -1;
    int token_fd = -1;
    char initial_token[64];
    ssize_t initial_token_length = 0;
    navigation_context_t context;
    bool context_created = false;

    // List retains its synthetic Arrow lifetime check. Column is launched only
    // after Karabiner's pressed-variable delayed condition and uses the
    // generation token written by monitor-start as its lifetime authority.
    if (view == edge_monitor_list_view
        && !arrow_key_is_down(direction)) {
        EDGE_TRACE("exit\tt=%.6f\treason=arrow-up-at-start\n", monotonic_seconds());
        goto cleanup;
    }
    if (!AXIsProcessTrusted()) {
        EDGE_TRACE("exit\tt=%.6f\treason=ax-untrusted\n", monotonic_seconds());
        result = 1;
        goto cleanup;
    }

    if (view == edge_monitor_column_view) {
        char path[PATH_MAX];
        token_path(path, sizeof(path), direction);
        token_fd = open(path, O_RDONLY);
        if (token_fd < 0) {
            EDGE_TRACE("exit\tt=%.6f\treason=token-open\n", monotonic_seconds());
            goto cleanup;
        }
        initial_token_length = read_token(
            token_fd,
            initial_token,
            sizeof(initial_token)
        );
        if (initial_token_length <= 0) {
            EDGE_TRACE("exit\tt=%.6f\treason=token-empty\n", monotonic_seconds());
            goto cleanup;
        }
    }

    movement_lock_fd = open_movement_lock();
    if (view == edge_monitor_column_view) {
        result = run_lightweight_column_edge_monitor(
            direction,
            token_fd,
            initial_token,
            initial_token_length,
            movement_lock_fd
        );
        goto cleanup;
    }

    EDGE_TRACE("context-start\tt=%.6f\n", monotonic_seconds());
    // This builds a read-only AX snapshot. Finder's direct Arrow traversal is
    // outside the Finer movement lock anyway, so taking the exclusive lock
    // here cannot make the snapshot atomic and can starve the observer behind
    // another short-lived worker. Writes at the actual wrap remain locked.
    context_created = navigation_context_create(&context);
    EDGE_TRACE("context-finish\tt=%.6f\n", monotonic_seconds());
    if (!context_created) {
        EDGE_TRACE("exit\tt=%.6f\treason=context-create\n", monotonic_seconds());
        result = 1;
        goto cleanup;
    }

    CFIndex item_count = CFArrayGetCount(context.items);
    if (context.role != navigation_outline
        || item_count < 2
        || !mark_state_is_empty()) {
        EDGE_TRACE(
            "exit\tt=%.6f\treason=context-guard\trole=%d\texpected=%d\titems=%ld\n",
            monotonic_seconds(),
            context.role,
            navigation_outline,
            item_count
        );
        goto cleanup;
    }

    CFIndex first = selectable_vertical_edge_index(&context, false);
    CFIndex last = selectable_vertical_edge_index(&context, true);
    CFIndex boundary = direction == direction_down ? last : first;
    CFIndex wrapped_target = direction == direction_down ? first : last;
    if (first < 0 || last < 0 || first == last) goto cleanup;
    EDGE_TRACE(
        "context\tt=%.6f\titems=%ld\tfirst=%ld\tlast=%ld\tboundary=%ld\ttarget=%ld\n",
        monotonic_seconds(),
        item_count,
        first,
        last,
        boundary,
        wrapped_target
    );
    vertical_edge_monitor_state_t monitor_state;
    vertical_edge_monitor_state_create(&monitor_state);
    // A 1,000-item Column can take more than 30 seconds with Finder's actual
    // repeat cadence. Keep a safety cap, but make it long enough for a full
    // 10,000-item traversal while still ending immediately on key-up or focus
    // loss.
    double deadline = monotonic_seconds() + 300.0;

    while (monotonic_seconds() < deadline) {
        if (!arrow_key_is_down(direction)
            || !process_is_frontmost(context.finder_pid)) {
            EDGE_TRACE("exit\tt=%.6f\treason=release-or-focus\n", monotonic_seconds());
            break;
        }

        CFIndex current = current_index(&context);
        if (current < 0) {
            usleep(50000);
            continue;
        }

        CFIndex distance = current > boundary
            ? current - boundary
            : boundary - current;
        bool viewport_available = false;
        bool viewport_near_boundary = false;
        if (current == boundary || distance > 12) {
            viewport_near_boundary = vertical_viewport_near_boundary(
                &context,
                direction,
                &viewport_available
            );
        }
        bool boundary_mismatch = current == boundary
            && viewport_available
            && !viewport_near_boundary;
        CFIndex observed = boundary_mismatch
            ? direction == direction_down ? boundary - 1 : boundary + 1
            : current;

        bool should_wrap = vertical_edge_monitor_observe(
            &monitor_state,
            observed,
            boundary
        );
        EDGE_TRACE(
            "observe\tt=%.6f\tcurrent=%ld\tobserved=%ld\tboundary=%ld\tviewport_available=%d\tviewport_near=%d\tmismatch=%d\tstable=%u\twrap=%d\n",
            monotonic_seconds(),
            current,
            observed,
            boundary,
            viewport_available,
            viewport_near_boundary,
            boundary_mismatch,
            monitor_state.stable_observations,
            should_wrap
        );
        if (should_wrap) {
            bool arrow_held = arrow_key_is_down(direction);
            bool finder_frontmost = process_is_frontmost(context.finder_pid);
            bool marks_empty = mark_state_is_empty();
            EDGE_TRACE(
                "guard\tt=%.6f\tarrow=%d\tfrontmost=%d\tmarks_empty=%d\n",
                monotonic_seconds(),
                arrow_held,
                finder_frontmost,
                marks_empty
            );
            if (!arrow_held
                || !finder_frontmost
                || !marks_empty) {
                break;
            }

            CFIndex wrapped_position = 0;
            if (movement_lock_fd >= 0) {
                flock(movement_lock_fd, LOCK_EX);
            }
            CFIndex confirmed = current_index(&context);
            if (confirmed == boundary
                && arrow_key_is_down(direction)
                && process_is_frontmost(context.finder_pid)) {
                wrapped_position = wrap_vertical_edge(
                    &context,
                    direction
                );
            }
            if (movement_lock_fd >= 0) {
                flock(movement_lock_fd, LOCK_UN);
            }
            if (wrapped_position == 0) {
                EDGE_TRACE("exit\tt=%.6f\treason=wrap-failed\n", monotonic_seconds());
                result = 1;
                break;
            }

            vertical_edge_monitor_mark_wrapped(
                &monitor_state,
                wrapped_target
            );
            EDGE_TRACE(
                "wrapped\tt=%.6f\tposition=%ld\tnext_delay_us=%llu\n",
                monotonic_seconds(),
                wrapped_position,
                (unsigned long long)vertical_edge_probe_delay_microseconds(
                    wrapped_target,
                    boundary
                )
            );
            // Do not read Finder selection immediately after the AX jump.
            // Let the still-held native Arrow own the entire next traversal,
            // and wait until even a conservative 120-steps/s stream could be
            // near the opposite boundary before asking Finder again.
            if (!wait_for_vertical_edge_delay(
                    &context,
                    direction,
                    vertical_edge_probe_delay_microseconds(
                        wrapped_target,
                        boundary
                    ),
                    deadline
                )) {
                break;
            }
            continue;
        }

        // No AX readback can be useful before even a 120-steps/s repeat could
        // reach the near-edge window. Sleep in short chunks so key release and
        // Finder focus are still noticed promptly, then recalculate from one
        // fresh selection observation. This keeps ordinary native movement
        // free of periodic Finder AX contention.
        if (!wait_for_vertical_edge_probe(
                &context,
                direction,
                current,
                boundary,
                viewport_near_boundary,
                boundary_mismatch,
                deadline
            )) {
            break;
        }
    }

cleanup:
    EDGE_TRACE("cleanup\tt=%.6f\tresult=%d\n", monotonic_seconds(), result);
#ifdef FINER_EDGE_TRACE
    if (edge_trace_fd >= 0) close(edge_trace_fd);
#endif
    if (token_fd >= 0) close(token_fd);
    if (movement_lock_fd >= 0) close(movement_lock_fd);
    if (context_created) navigation_context_release(&context);
    flock(monitor_lock_fd, LOCK_UN);
    close(monitor_lock_fd);
    return result;
}
