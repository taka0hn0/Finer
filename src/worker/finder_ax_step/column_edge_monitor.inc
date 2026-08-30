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
