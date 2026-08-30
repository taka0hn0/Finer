// Shared lifetime for both delayed edge monitors: the per-view lock, the AX
// trust check, the Column generation token, and one cleanup path. The actual
// watching lives in column_edge_monitor.inc and list_edge_monitor.inc.
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

    result = run_list_edge_monitor_loop(
        &context,
        direction,
        movement_lock_fd
    );

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
