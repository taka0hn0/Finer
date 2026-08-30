// Scaffolding shared by the CLI commands. `main` stays a dispatch table and
// each command keeps only the work that makes it different.

static bool require_accessibility(void) {
    if (AXIsProcessTrusted()) return true;
    fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
    return false;
}

// Commands that only read Finder state build a context without the movement
// lock, so they cannot stall a transient worker mid-burst.
static bool begin_readonly_context(navigation_context_t *context) {
    return require_accessibility() && navigation_context_create(context);
}

// Commands that change the selection serialize against the transient worker
// through the movement lock, and hold it until the context is released.
static bool begin_locked_movement(
    navigation_context_t *context,
    int *lock_fd
) {
    *lock_fd = -1;
    if (!require_accessibility()) return false;

    *lock_fd = open_movement_lock();
    if (*lock_fd >= 0) flock(*lock_fd, LOCK_EX);
    if (navigation_context_create(context)) return true;

    if (*lock_fd >= 0) {
        close(*lock_fd);
        *lock_fd = -1;
    }
    return false;
}

static void end_locked_movement(navigation_context_t *context, int lock_fd) {
    navigation_context_release(context);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

// A movement command answers with the 1-based position of the new selection,
// and reports failure through the exit status rather than on stdout.
static int print_movement_position(CFIndex position) {
    if (position <= 0) return 1;
    printf("%ld\n", position);
    return 0;
}

static int run_worker_entry_command(char **argv) {
    char worker_command;
    direction_t direction;
    if (strcmp(argv[2], "clear-selection") == 0) {
        worker_command = 'e';
    } else if (strcmp(argv[2], "toggle-mark") == 0) {
        worker_command = 's';
    } else if (parse_direction(argv[2], &direction)) {
        worker_command = direction_command(direction);
    } else {
        return 64;
    }

    char *end = NULL;
    long worker_lock_fd = strtol(argv[3], &end, 10);
    if (!end || *end != '\0' || worker_lock_fd < 0 || worker_lock_fd > INT_MAX) {
        return 64;
    }
    end = NULL;
    unsigned long long submitted_nanoseconds = strtoull(argv[4], &end, 10);
    if (!end || *end != '\0' || submitted_nanoseconds == 0) return 64;

    return run_worker(
        worker_command,
        (int)worker_lock_fd,
        submitted_nanoseconds
    );
}

static int run_notify_key_command(char **argv) {
    char *ends[4] = {NULL, NULL, NULL, NULL};
    errno = 0;
    unsigned long key_code = strtoul(argv[2], &ends[0], 10);
    unsigned long long modifiers = strtoull(argv[3], &ends[1], 10);
    long suppress_key_code = strtol(argv[4], &ends[2], 10);
    unsigned long long suppress_modifiers = strtoull(argv[5], &ends[3], 10);
    if (errno != 0
        || !ends[0] || *ends[0] != '\0'
        || !ends[1] || *ends[1] != '\0'
        || !ends[2] || *ends[2] != '\0'
        || !ends[3] || *ends[3] != '\0'
        || key_code > UINT16_MAX
        || suppress_key_code < -1
        || suppress_key_code > UINT16_MAX) {
        return 64;
    }
    notify_keystroke(
        (uint16_t)key_code,
        (uint64_t)modifiers,
        (int)suppress_key_code,
        (uint64_t)suppress_modifiers
    );
    return 0;
}

static int run_vertical_scroll_value_command(void) {
    navigation_context_t context;
    if (!begin_readonly_context(&context)) return 1;
    double value = 0.0;
    bool available = vertical_scroll_value(&context, &value);
    navigation_context_release(&context);
    if (!available) return 1;
    printf("%.6f\n", value);
    return 0;
}

static int run_selected_visible_command(void) {
    navigation_context_t context;
    if (!begin_readonly_context(&context)) return 1;
    bool visible = selected_item_is_visible(&context);
    navigation_context_release(&context);
    printf("%d\n", visible ? 1 : 0);
    return visible ? 0 : 1;
}

static int run_scroll_state_command(void) {
    navigation_context_t context;
    if (!begin_readonly_context(&context)) return 1;
    CFIndex current = current_index(&context);
    fast_ax_scroll_state_t state;
    fast_ax_scroll_state_create(&context, current, &state);
    printf(
        "current=%ld first=%ld last=%ld indicator=%d increment=%d decrement=%d\n",
        current,
        state.first_visible,
        state.last_visible,
        state.value_indicator != NULL,
        state.increment_button != NULL,
        state.decrement_button != NULL
    );
    fast_ax_scroll_state_release(&state);
    navigation_context_release(&context);
    return 0;
}

static int run_edge_command(bool last) {
    navigation_context_t context;
    int lock_fd = -1;
    if (!begin_locked_movement(&context, &lock_fd)) return 1;
    CFIndex position = move_to_edge(&context, last);
    end_locked_movement(&context, lock_fd);
    return print_movement_position(position);
}

static int run_wrap_command(direction_t direction) {
    navigation_context_t context;
    int lock_fd = -1;
    if (!begin_locked_movement(&context, &lock_fd)) return 1;
    CFIndex position = move_once(
        &context,
        direction,
        current_index_unknown,
        -1,
        NULL,
        true,
        NULL
    );
    end_locked_movement(&context, lock_fd);
    return print_movement_position(position);
}

static int run_count_move_command(direction_t direction, const char *count) {
    if (direction != direction_down && direction != direction_up) return 64;

    char *end = NULL;
    errno = 0;
    long requested_count = strtol(count, &end, 10);
    if (errno != 0 || !end || *end != '\0'
        || requested_count < 1 || requested_count > 99) {
        return 64;
    }

    navigation_context_t context;
    int lock_fd = -1;
    if (!begin_locked_movement(&context, &lock_fd)) return 1;
    CFIndex position = move_vertical_count(
        &context,
        direction,
        (CFIndex)requested_count
    );
    end_locked_movement(&context, lock_fd);
    return print_movement_position(position);
}

static int run_self_test_command(const char *name) {
    if (strcmp(name, "edge-monitor-self-test") == 0) {
        int result = run_vertical_edge_monitor_state_self_test();
        return result == 0
            ? run_column_selection_stall_state_self_test()
            : result;
    }
    if (strcmp(name, "edge-monitor-lock-self-test") == 0) {
        return run_vertical_edge_monitor_lock_self_test();
    }
    if (strcmp(name, "hold-token-stop-self-test") == 0) {
        return run_hold_token_stop_self_test();
    }
    if (strcmp(name, "vertical-hold-state-self-test") == 0) {
        return run_vertical_hold_state_self_test();
    }
    return -1;
}

static int run_directional_command(const char *name, direction_t direction) {
    if (strcmp(name, "list-edge-monitor-worker") == 0) {
        return run_vertical_edge_monitor_worker(
            edge_monitor_list_view,
            direction
        );
    }
    if (strcmp(name, "list-edge-monitor-start") == 0) {
        return spawn_vertical_edge_monitor(
            edge_monitor_list_view,
            direction
        ) ? 0 : 1;
    }
    if (strcmp(name, "column-edge-monitor-worker") == 0) {
        return run_vertical_edge_monitor_worker(
            edge_monitor_column_view,
            direction
        );
    }
    if (strcmp(name, "column-edge-monitor-start") == 0) {
        return spawn_vertical_edge_monitor(
            edge_monitor_column_view,
            direction
        ) ? 0 : 1;
    }
    if (strcmp(name, "hold-start") == 0) return run_hold_start(direction);
    if (strcmp(name, "hold-token-start") == 0) {
        return run_hold_token_start(direction);
    }
    if (strcmp(name, "hold-stop") == 0) {
        return stop_hold_token(direction) ? 0 : 1;
    }
    if (strcmp(name, "hold-stop-now") == 0) {
        return clear_hold_token(direction) ? 0 : 1;
    }
    if (strcmp(name, "hold-repeat") == 0) {
        if (!require_accessibility()) return 1;
        return run_hold_repeat(direction);
    }
    return -1;
}
