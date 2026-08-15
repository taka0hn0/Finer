static void column_edge_context_release(column_edge_context_t *context) {
    if (context->container) CFRelease(context->container);
    memset(context, 0, sizeof(*context));
}

static bool column_edge_context_create(
    column_edge_context_t *context
) {
    memset(context, 0, sizeof(*context));
    AXUIElementRef application = NULL;
    if (!finder_is_frontmost(&context->finder_pid, &application)) {
        return false;
    }

    CFTypeRef focused = copy_attribute(
        application,
        kAXFocusedUIElementAttribute
    );
    CFRelease(application);
    if (!focused || CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        return false;
    }

    navigation_role_t role;
    context->container = copy_nearest_navigation_container(
        (AXUIElementRef)focused,
        &role
    );
    CFRelease(focused);
    if (!context->container || role != navigation_list) {
        column_edge_context_release(context);
        return false;
    }
    return true;
}

static bool post_column_vertical_wrap(
    const column_edge_context_t *context,
    direction_t direction,
    int movement_lock_fd
) {
    if (movement_lock_fd >= 0) {
        flock(movement_lock_fd, LOCK_EX);
    }
    bool wrapped = post_vertical_wrap_chord(
        context->finder_pid,
        direction
    );
    if (movement_lock_fd >= 0) {
        flock(movement_lock_fd, LOCK_UN);
    }
    return wrapped;
}

static int run_owned_column_tap_repeat(
    const column_edge_context_t *context,
    direction_t direction,
    int token_fd,
    const char *initial_token,
    ssize_t initial_token_length,
    column_selection_stall_state_t *stall_state,
    double deadline,
    int movement_lock_fd
) {
    useconds_t interval = column_owned_hold_interval_microseconds();
    uint64_t interval_ticks = mach_ticks_for_microseconds(interval);
    uint64_t probe_ticks = mach_ticks_for_microseconds(
        column_selection_probe_interval_microseconds()
    );
    uint64_t next_step = mach_absolute_time();
    uint64_t next_probe = next_step + probe_ticks;
    CGKeyCode key_code = arrow_key_code(direction);
    int result = 0;

    while (monotonic_seconds() < deadline) {
        uint64_t now = mach_absolute_time();
        if (now < next_step) mach_wait_until(next_step);
        if (monotonic_seconds() >= deadline
            || !hold_token_matches(
                token_fd,
                initial_token,
                initial_token_length
            )
            || !process_is_frontmost(context->finder_pid)) {
            break;
        }

        if (movement_lock_fd >= 0) {
            flock(movement_lock_fd, LOCK_EX);
        }
        bool posted = post_key_code(key_code);
        if (movement_lock_fd >= 0) {
            flock(movement_lock_fd, LOCK_UN);
        }
        if (!posted) {
            result = 1;
            break;
        }

        bool wrapped = false;
        if (mach_absolute_time() >= next_probe) {
            AXUIElementRef current = copy_selected_column_child(
                context->container
            );
            bool stalled = current
                && column_selection_stall_state_observe(
                    stall_state,
                    current
                );
            if (current) CFRelease(current);
            EDGE_TRACE(
                "observe\tt=%.6f\tkind=column-stall\tstalled=%d\n",
                monotonic_seconds(),
                stalled
            );

            if (stalled) {
                bool token_current = hold_token_matches(
                    token_fd,
                    initial_token,
                    initial_token_length
                );
                bool finder_frontmost = process_is_frontmost(
                    context->finder_pid
                );
                bool marks_empty = mark_state_is_empty();
                if (!token_current || !finder_frontmost || !marks_empty) {
                    break;
                }
                wrapped = post_column_vertical_wrap(
                    context,
                    direction,
                    movement_lock_fd
                );
                if (!wrapped) {
                    result = 1;
                    break;
                }
                EDGE_TRACE(
                    "wrapped\tt=%.6f\tkind=column-stall\n",
                    monotonic_seconds()
                );
            }
            next_probe = mach_absolute_time() + probe_ticks;
        }

        if (wrapped) {
            next_step = mach_absolute_time() + interval_ticks;
            continue;
        }
        next_step += interval_ticks;
        uint64_t finished = mach_absolute_time();
        if (finished >= next_step) {
            uint64_t missed =
                (finished - next_step) / interval_ticks + 1;
            next_step += missed * interval_ticks;
        }
    }
    return result;
}

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

static int run_vertical_edge_wrap_test(
    edge_monitor_view_t view,
    direction_t direction
) {
    if (direction != direction_down && direction != direction_up) return 64;
    if (!AXIsProcessTrusted()) {
        fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
        return 1;
    }

    int result = 1;
    int movement_lock_fd = open_movement_lock();
    navigation_context_t context;
    bool context_created = false;
    if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_EX);
    context_created = navigation_context_create(&context);
    if (!context_created) goto cleanup;

    navigation_role_t expected_role = view == edge_monitor_list_view
        ? navigation_outline
        : navigation_list;
    CFIndex item_count = CFArrayGetCount(context.items);
    CFIndex boundary = selectable_vertical_edge_index(
        &context,
        direction == direction_down
    );
    CFIndex current = current_index(&context);
    bool marks_empty = mark_state_is_empty();
    if (context.role != expected_role
        || item_count < 2
        || !marks_empty
        || current != boundary) {
        fprintf(
            stderr,
            "edge-wrap-test: role=%d expected=%d items=%ld current=%ld boundary=%ld marks_empty=%d\n",
            context.role,
            expected_role,
            item_count,
            current,
            boundary,
            marks_empty
        );
        goto cleanup;
    }

    CFIndex position;
    if (view == edge_monitor_column_view) {
        CFIndex target = direction == direction_down ? 0 : item_count - 1;
        position = post_vertical_wrap_chord(
            context.finder_pid,
            direction
        )
            ? target + 1
            : 0;
    } else {
        position = wrap_vertical_edge(&context, direction);
    }
    // Grouped List views may expose a non-selectable heading at an edge, so
    // the AX array position is not necessarily 1 or item_count. The Finder
    // integration test verifies the selected file path after either the List
    // AX action or the Column native Option+Arrow action.
    if (position <= 0) goto cleanup;
    printf("%ld\n", position);
    result = 0;

cleanup:
    if (context_created) navigation_context_release(&context);
    if (movement_lock_fd >= 0) {
        flock(movement_lock_fd, LOCK_UN);
        close(movement_lock_fd);
    }
    return result;
}

static bool spawn_vertical_edge_monitor(
    edge_monitor_view_t view,
    direction_t direction
) {
    if (direction != direction_down && direction != direction_up) return false;

    // Karabiner reliably launches this command after its hold threshold, but
    // a separate shell command in the initial key-down action is not a safe
    // hand-off point: the worker can otherwise observe an empty token and
    // exit before it ever inspects Finder.  Create the generation token here,
    // immediately before the Column monitor is spawned.  Key-up still
    // truncates the same file, so the takeover repeat stops with physical j/k.
    if (view == edge_monitor_column_view
        && !write_hold_token(direction)) {
        return false;
    }

    const char *direction_value = direction == direction_down ? "down" : "up";
    const char *worker_command = view == edge_monitor_list_view
        ? "list-edge-monitor-worker"
        : "column-edge-monitor-worker";
    char *arguments[] = {
        (char *)program_path,
        (char *)worker_command,
        (char *)direction_value,
        NULL,
    };

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDIN_FILENO,
        "/dev/null",
        O_RDONLY,
        0
    );
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDOUT_FILENO,
        "/dev/null",
        O_WRONLY,
        0
    );
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDERR_FILENO,
        "/dev/null",
        O_WRONLY,
        0
    );

    pid_t child;
    int spawn_error = posix_spawn(
        &child,
        program_path,
        &file_actions,
        NULL,
        arguments,
        environ
    );
    posix_spawn_file_actions_destroy(&file_actions);
    return spawn_error == 0;
}
