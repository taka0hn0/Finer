static int open_vertical_edge_monitor_lock(
    edge_monitor_view_t view,
    direction_t direction
) {
    char path[PATH_MAX];
    const char *view_name = view == edge_monitor_list_view
        ? "list"
        : "column";
    const char *direction_name = direction == direction_down ? "down" : "up";
    char name[64];
    snprintf(
        name,
        sizeof(name),
        "finder_%s_%s_edge_monitor.lock",
        view_name,
        direction_name
    );
    state_path(path, sizeof(path), name);
    return open(path, O_CREAT | O_RDWR, 0600);
}

static bool acquire_vertical_edge_monitor_lock(
    int lock_fd,
    bool allow_previous_column_handoff
) {
    double deadline = monotonic_seconds() + 0.25;
    for (;;) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0) return true;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
            return false;
        }
        if (!allow_previous_column_handoff
            || monotonic_seconds() >= deadline) {
            return false;
        }
        usleep(2000);
    }
}

#ifdef FINER_EDGE_TRACE
static void open_edge_trace(
    edge_monitor_view_t view,
    direction_t direction
) {
    char path[PATH_MAX];
    char name[96];
    snprintf(
        name,
        sizeof(name),
        "finder_%s_%s_edge_monitor_trace.tsv",
        view == edge_monitor_list_view ? "list" : "column",
        direction == direction_down ? "down" : "up"
    );
    state_path(path, sizeof(path), name);
    edge_trace_fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (edge_trace_fd < 0) return;
    dprintf(
        edge_trace_fd,
        "start\tt=%.6f\tpid=%ld\tview=%d\tdirection=%d\tarrow=%d\n",
        monotonic_seconds(),
        (long)getpid(),
        view,
        direction,
        arrow_key_is_down(direction)
    );
}

#endif

static int run_vertical_edge_monitor_lock_self_test(void) {
    const edge_monitor_view_t views[] = {
        edge_monitor_list_view,
        edge_monitor_column_view,
    };
    const direction_t directions[] = {
        direction_down,
        direction_up,
    };

    for (size_t view_index = 0;
            view_index < sizeof(views) / sizeof(views[0]);
            ++view_index) {
        for (size_t direction_index = 0;
                direction_index < sizeof(directions) / sizeof(directions[0]);
                ++direction_index) {
            int lock_fd = open_vertical_edge_monitor_lock(
                views[view_index],
                directions[direction_index]
            );
            if (lock_fd < 0) {
                fprintf(stderr, "edge monitor lock self-test: open failed\n");
                return 1;
            }
            close(lock_fd);
        }
    }

    int previous_fd = open_vertical_edge_monitor_lock(
        edge_monitor_column_view,
        direction_down
    );
    if (previous_fd < 0
        || !acquire_vertical_edge_monitor_lock(previous_fd, false)) {
        if (previous_fd >= 0) close(previous_fd);
        fprintf(stderr, "edge monitor lock self-test: setup failed\n");
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        flock(previous_fd, LOCK_UN);
        close(previous_fd);
        fprintf(stderr, "edge monitor lock self-test: fork failed\n");
        return 1;
    }
    if (child == 0) {
        int next_fd = open_vertical_edge_monitor_lock(
            edge_monitor_column_view,
            direction_down
        );
        bool acquired = next_fd >= 0
            && acquire_vertical_edge_monitor_lock(next_fd, true);
        if (acquired) flock(next_fd, LOCK_UN);
        if (next_fd >= 0) close(next_fd);
        _exit(acquired ? 0 : 1);
    }

    usleep(20000);
    flock(previous_fd, LOCK_UN);
    close(previous_fd);
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child
        || !WIFEXITED(child_status)
        || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "edge monitor lock self-test: handoff failed\n");
        return 1;
    }

    printf("Vertical edge monitor lock tests passed.\n");
    return 0;
}
