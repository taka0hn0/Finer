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
