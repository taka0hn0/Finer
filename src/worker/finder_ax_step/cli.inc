static void print_usage(void) {
    fprintf(
        stderr,
        "Usage: finder_ax_step <clear-selection|toggle-mark|edge-monitor-self-test|edge-monitor-lock-self-test|hold-token-stop-self-test|vertical-hold-state-self-test|vertical-scroll-value|selected-visible|scroll-state|first|last|down-wrap|up-wrap|left-wrap|right-wrap> | "
        "notify-key <key-code> <modifiers> <suppressed-key-code|-1> <suppressed-modifiers> | "
        "count-move <down|up> <1...99> | "
        "<hold-start|hold-token-start|hold-stop|hold-stop-now|hold-repeat> <down|up|left|right> | "
        "<list-edge-monitor-start|column-edge-monitor-start> <down|up> | "
        "edge-wrap-test <list|column> <down|up>\n"
    );
}

int main(int argc, char **argv) {
    program_path = argv[0];
    if (!configure_metrics()) return 64;

    if (argc == 5 && strcmp(argv[1], "worker") == 0) {
        return run_worker_entry_command(argv);
    }

    if (argc == 6 && strcmp(argv[1], "notify-key") == 0) {
        return run_notify_key_command(argv);
    }

    if (argc == 2) {
        const char *command = argv[1];
        if (strcmp(command, "toggle-mark") == 0) {
            return enqueue_toggle_mark() ? 0 : 1;
        }
        if (strcmp(command, "clear-selection") == 0) {
            return perform_finder_deselect_menu_action()
                    || enqueue_clear_selection()
                ? 0
                : 1;
        }
        if (strcmp(command, "vertical-scroll-value") == 0) {
            return run_vertical_scroll_value_command();
        }
        if (strcmp(command, "selected-visible") == 0) {
            return run_selected_visible_command();
        }
        if (strcmp(command, "scroll-state") == 0) {
            return run_scroll_state_command();
        }
        if (strcmp(command, "first") == 0) return run_edge_command(false);
        if (strcmp(command, "last") == 0) return run_edge_command(true);

        size_t length = strlen(command);
        const char wrap_suffix[] = "-wrap";
        if (length > sizeof(wrap_suffix) - 1
            && strcmp(command + length - (sizeof(wrap_suffix) - 1),
                      wrap_suffix) == 0) {
            char name[16];
            size_t name_length = length - (sizeof(wrap_suffix) - 1);
            if (name_length < sizeof(name)) {
                memcpy(name, command, name_length);
                name[name_length] = '\0';
                direction_t direction;
                if (parse_direction(name, &direction)) {
                    return run_wrap_command(direction);
                }
            }
        }

        int self_test_result = run_self_test_command(command);
        if (self_test_result >= 0) return self_test_result;
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
        direction_t direction;
        if (!parse_direction(argv[2], &direction)) return 64;
        return run_count_move_command(direction, argv[3]);
    }

    if (argc == 3) {
        direction_t direction;
        if (!parse_direction(argv[2], &direction)) return 64;
        int result = run_directional_command(argv[1], direction);
        if (result >= 0) return result;
    }

    print_usage();
    return 64;
}
