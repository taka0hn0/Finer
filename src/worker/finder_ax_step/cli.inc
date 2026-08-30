int main(int argc, char **argv) {
    program_path = argv[0];
    if (!configure_metrics()) return 64;

    if (argc == 5 && strcmp(argv[1], "worker") == 0) {
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

    if (argc == 2 && strcmp(argv[1], "toggle-mark") == 0) {
        return enqueue_toggle_mark() ? 0 : 1;
    }

    if (argc == 2 && strcmp(argv[1], "clear-selection") == 0) {
        return perform_finder_deselect_menu_action() || enqueue_clear_selection()
            ? 0
            : 1;
    }

    if (argc == 6 && strcmp(argv[1], "notify-key") == 0) {
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

    if (argc == 2 && strcmp(argv[1], "edge-monitor-self-test") == 0) {
        int result = run_vertical_edge_monitor_state_self_test();
        return result == 0
            ? run_column_selection_stall_state_self_test()
            : result;
    }

    if (argc == 2 && strcmp(argv[1], "edge-monitor-lock-self-test") == 0) {
        return run_vertical_edge_monitor_lock_self_test();
    }

    if (argc == 2 && strcmp(argv[1], "hold-token-stop-self-test") == 0) {
        return run_hold_token_stop_self_test();
    }

    if (argc == 2 && strcmp(argv[1], "vertical-hold-state-self-test") == 0) {
        return run_vertical_hold_state_self_test();
    }

    if (argc == 2 && strcmp(argv[1], "vertical-scroll-value") == 0) {
        if (!AXIsProcessTrusted()) {
            fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
            return 1;
        }
        navigation_context_t context;
        if (!navigation_context_create(&context)) return 1;
        double value = 0.0;
        bool available = vertical_scroll_value(&context, &value);
        navigation_context_release(&context);
        if (!available) return 1;
        printf("%.6f\n", value);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "selected-visible") == 0) {
        if (!AXIsProcessTrusted()) {
            fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
            return 1;
        }
        navigation_context_t context;
        if (!navigation_context_create(&context)) return 1;
        bool visible = selected_item_is_visible(&context);
        navigation_context_release(&context);
        printf("%d\n", visible ? 1 : 0);
        return visible ? 0 : 1;
    }

    if (argc == 2 && strcmp(argv[1], "scroll-state") == 0) {
        if (!AXIsProcessTrusted()) {
            fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
            return 1;
        }
        navigation_context_t context;
        if (!navigation_context_create(&context)) return 1;
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

    if (argc == 2 && (strcmp(argv[1], "first") == 0
            || strcmp(argv[1], "last") == 0)) {
        if (!AXIsProcessTrusted()) {
            fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
            return 1;
        }
        navigation_context_t context;
        int lock_fd = open_movement_lock();
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        if (!navigation_context_create(&context)) {
            if (lock_fd >= 0) close(lock_fd);
            return 1;
        }
        CFIndex position = move_to_edge(
            &context,
            strcmp(argv[1], "last") == 0
        );
        navigation_context_release(&context);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        if (position == 0) return 1;
        printf("%ld\n", position);
        return 0;
    }

    if (argc == 2 && (strcmp(argv[1], "down-wrap") == 0
            || strcmp(argv[1], "up-wrap") == 0
            || strcmp(argv[1], "left-wrap") == 0
            || strcmp(argv[1], "right-wrap") == 0)) {
        if (!AXIsProcessTrusted()) {
            fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
            return 1;
        }
        direction_t direction;
        if (strcmp(argv[1], "down-wrap") == 0) {
            direction = direction_down;
        } else if (strcmp(argv[1], "up-wrap") == 0) {
            direction = direction_up;
        } else if (strcmp(argv[1], "left-wrap") == 0) {
            direction = direction_left;
        } else {
            direction = direction_right;
        }
        navigation_context_t context;
        int lock_fd = open_movement_lock();
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        if (!navigation_context_create(&context)) {
            if (lock_fd >= 0) close(lock_fd);
            return 1;
        }
        CFIndex position = move_once(
            &context,
            direction,
            current_index_unknown,
            -1,
            NULL,
            true,
            NULL
        );
        navigation_context_release(&context);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        if (position == 0) return 1;
        printf("%ld\n", position);
        return 0;
    }

    if (argc == 4 && strcmp(argv[1], "edge-wrap-test") == 0) {
        edge_monitor_view_t view;
        if (strcmp(argv[2], "list") == 0) {
            view = edge_monitor_list_view;
        } else if (strcmp(argv[2], "column") == 0) {
            view = edge_monitor_column_view;
        } else {
            return 64;
        }
        direction_t direction;
        if (!parse_direction(argv[3], &direction)) return 64;
        return run_vertical_edge_wrap_test(view, direction);
    }

    if (argc == 4 && strcmp(argv[1], "count-move") == 0) {
        if (!AXIsProcessTrusted()) {
            fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
            return 1;
        }
        direction_t direction;
        if (!parse_direction(argv[2], &direction)
            || (direction != direction_down && direction != direction_up)) {
            return 64;
        }
        char *end = NULL;
        errno = 0;
        long requested_count = strtol(argv[3], &end, 10);
        if (errno != 0 || !end || *end != '\0'
            || requested_count < 1 || requested_count > 99) {
            return 64;
        }

        navigation_context_t context;
        int lock_fd = open_movement_lock();
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        bool context_created = navigation_context_create(&context);
        CFIndex position = context_created
            ? move_vertical_count(
                &context,
                direction,
                (CFIndex)requested_count
            )
            : 0;
        if (context_created) navigation_context_release(&context);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        if (position <= 0) return 1;
        printf("%ld\n", position);
        return 0;
    }

    if (argc == 3) {
        direction_t direction;
        if (!parse_direction(argv[2], &direction)) return 64;
        if (strcmp(argv[1], "list-edge-monitor-worker") == 0) {
            return run_vertical_edge_monitor_worker(
                edge_monitor_list_view,
                direction
            );
        }
        if (strcmp(argv[1], "list-edge-monitor-start") == 0) {
            return spawn_vertical_edge_monitor(
                edge_monitor_list_view,
                direction
            ) ? 0 : 1;
        }
        if (strcmp(argv[1], "column-edge-monitor-worker") == 0) {
            return run_vertical_edge_monitor_worker(
                edge_monitor_column_view,
                direction
            );
        }
        if (strcmp(argv[1], "column-edge-monitor-start") == 0) {
            return spawn_vertical_edge_monitor(
                edge_monitor_column_view,
                direction
            ) ? 0 : 1;
        }
        if (strcmp(argv[1], "hold-start") == 0) return run_hold_start(direction);
        if (strcmp(argv[1], "hold-token-start") == 0) {
            return run_hold_token_start(direction);
        }
        if (strcmp(argv[1], "hold-stop") == 0) {
            return stop_hold_token(direction) ? 0 : 1;
        }
        if (strcmp(argv[1], "hold-stop-now") == 0) {
            return clear_hold_token(direction) ? 0 : 1;
        }
        if (strcmp(argv[1], "hold-repeat") == 0) {
            if (!AXIsProcessTrusted()) {
                fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
                return 1;
            }
            return run_hold_repeat(direction);
        }
    }

    fprintf(
        stderr,
        "Usage: finder_ax_step <clear-selection|toggle-mark|edge-monitor-self-test|edge-monitor-lock-self-test|hold-token-stop-self-test|vertical-hold-state-self-test|vertical-scroll-value|selected-visible|scroll-state|first|last|down-wrap|up-wrap|left-wrap|right-wrap> | "
        "notify-key <key-code> <modifiers> <suppressed-key-code|-1> <suppressed-modifiers> | "
        "count-move <down|up> <1...99> | "
        "<hold-start|hold-token-start|hold-stop|hold-stop-now|hold-repeat> <down|up|left|right> | "
        "<list-edge-monitor-start|column-edge-monitor-start> <down|up> | "
        "edge-wrap-test <list|column> <down|up>\n"
    );
    return 64;
}
