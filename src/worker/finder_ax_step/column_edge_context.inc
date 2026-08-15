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
