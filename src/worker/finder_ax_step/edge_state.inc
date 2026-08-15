static void vertical_edge_monitor_state_create(
    vertical_edge_monitor_state_t *state
) {
    memset(state, 0, sizeof(*state));
    state->previous = -1;
}

static bool vertical_edge_monitor_observe(
    vertical_edge_monitor_state_t *state,
    CFIndex current,
    CFIndex boundary
) {
    if (current == boundary) {
        state->stable_observations = state->previous == current
            ? state->stable_observations + 1
            : 1;
        state->previous = current;
        return state->stable_observations >= 2;
    }

    state->stable_observations = 0;
    state->previous = current;
    return false;
}

static void vertical_edge_monitor_mark_wrapped(
    vertical_edge_monitor_state_t *state,
    CFIndex wrapped_target
) {
    state->stable_observations = 0;
    state->previous = wrapped_target;
}

static uint64_t vertical_edge_probe_delay_microseconds(
    CFIndex current,
    CFIndex boundary
) {
    const CFIndex near_edge_items = 12;
    const uint64_t fastest_step_microseconds = 8333;
    CFIndex distance = current > boundary
        ? current - boundary
        : boundary - current;
    if (distance <= near_edge_items) return fastest_step_microseconds;
    return (uint64_t)(distance - near_edge_items)
        * fastest_step_microseconds;
}

static int run_vertical_edge_monitor_state_self_test(void) {
    vertical_edge_monitor_state_t state;
    vertical_edge_monitor_state_create(&state);

    if (vertical_edge_monitor_observe(&state, 4, 9)
        || vertical_edge_monitor_observe(&state, 9, 9)
        || !vertical_edge_monitor_observe(&state, 9, 9)) {
        fprintf(stderr, "edge monitor self-test: stable boundary failed\n");
        return 1;
    }

    vertical_edge_monitor_mark_wrapped(&state, 0);
    if (vertical_edge_monitor_observe(&state, 0, 9)
        || state.stable_observations != 0
        || vertical_edge_monitor_observe(&state, 1, 9)
        || state.stable_observations != 0) {
        fprintf(stderr, "edge monitor self-test: rearm failed\n");
        return 1;
    }

    if (vertical_edge_monitor_observe(&state, 9, 9)
        || vertical_edge_monitor_observe(&state, 8, 9)
        || vertical_edge_monitor_observe(&state, 9, 9)
        || !vertical_edge_monitor_observe(&state, 9, 9)) {
        fprintf(stderr, "edge monitor self-test: reset failed\n");
        return 1;
    }

    vertical_edge_monitor_state_create(&state);
    if (vertical_edge_monitor_observe(&state, 3, 0)
        || vertical_edge_monitor_observe(&state, 0, 0)
        || !vertical_edge_monitor_observe(&state, 0, 0)) {
        fprintf(stderr, "edge monitor self-test: reverse boundary failed\n");
        return 1;
    }

    if (vertical_edge_probe_delay_microseconds(0, 0) != 8333
        || vertical_edge_probe_delay_microseconds(12, 0) != 8333
        || vertical_edge_probe_delay_microseconds(112, 0) != 833300
        || vertical_edge_probe_delay_microseconds(0, 112) != 833300) {
        fprintf(stderr, "edge monitor self-test: probe delay failed\n");
        return 1;
    }

    printf("Vertical edge monitor state tests passed.\n");
    return 0;
}

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

static bool wait_for_vertical_edge_delay(
    const navigation_context_t *context,
    direction_t direction,
    uint64_t remaining_microseconds,
    double deadline
) {
    while (remaining_microseconds > 0) {
        if (!arrow_key_is_down(direction)
            || !process_is_frontmost(context->finder_pid)) {
            return false;
        }

        double deadline_seconds = deadline - monotonic_seconds();
        if (deadline_seconds <= 0) return false;
        uint64_t deadline_microseconds = (uint64_t)(
            deadline_seconds * 1000000.0
        );
        uint64_t chunk = remaining_microseconds;
        if (chunk > 50000) chunk = 50000;
        if (chunk > deadline_microseconds) chunk = deadline_microseconds;
        if (chunk == 0) return false;
        usleep((useconds_t)chunk);
        remaining_microseconds -= chunk;
    }
    return true;
}

static bool wait_for_vertical_edge_probe(
    const navigation_context_t *context,
    direction_t direction,
    CFIndex current,
    CFIndex boundary,
    bool viewport_near_boundary,
    bool boundary_mismatch,
    double deadline
) {
    uint64_t delay_microseconds = boundary_mismatch
        ? 50000
        : viewport_near_boundary
            ? 8333
            : vertical_edge_probe_delay_microseconds(current, boundary);
    return wait_for_vertical_edge_delay(
        context,
        direction,
        delay_microseconds,
        deadline
    );
}

static bool vertical_viewport_near_boundary(
    const navigation_context_t *context,
    direction_t direction,
    bool *available
) {
    CFStringRef visible_attribute = context->role == navigation_outline
        ? kAXVisibleRowsAttribute
        : kAXVisibleChildrenAttribute;
    CFArrayRef visible = copy_array_attribute(
        context->container,
        visible_attribute
    );
    if (visible) {
        CFIndex item_count = CFArrayGetCount(context->items);
        CFIndex visible_count = CFArrayGetCount(visible);
        CFRelease(visible);
        if (item_count > 0 && visible_count >= item_count) {
            // Finder leaves a non-scrollable bar at value 0.0 when every item
            // fits in the viewport.  In that case either document boundary is
            // also a visible viewport boundary, so the selected edge item is
            // sufficient corroboration for wrapping.
            *available = true;
            return true;
        }
    }

    double value = 0;
    *available = vertical_scroll_value(context, &value);
    if (!*available) return false;
    return direction == direction_down ? value >= 0.98 : value <= 0.02;
}

typedef struct {
    AXUIElementRef container;
    pid_t finder_pid;
} column_edge_context_t;

typedef struct {
    CFTypeRef previous;
} column_selection_stall_state_t;

static void column_selection_stall_state_create(
    column_selection_stall_state_t *state
) {
    memset(state, 0, sizeof(*state));
}

static void column_selection_stall_state_release(
    column_selection_stall_state_t *state
) {
    if (state->previous) CFRelease(state->previous);
    memset(state, 0, sizeof(*state));
}

static bool column_selection_stall_state_observe(
    column_selection_stall_state_t *state,
    CFTypeRef current
) {
    if (!current) return false;
    bool stalled = state->previous && CFEqual(state->previous, current);
    if (!stalled) {
        if (state->previous) CFRelease(state->previous);
        state->previous = CFRetain(current);
    }
    return stalled;
}

static int run_column_selection_stall_state_self_test(void) {
    column_selection_stall_state_t state;
    column_selection_stall_state_create(&state);
    bool failed = column_selection_stall_state_observe(&state, CFSTR("A"))
        || column_selection_stall_state_observe(&state, CFSTR("B"))
        || !column_selection_stall_state_observe(&state, CFSTR("B"))
        || column_selection_stall_state_observe(&state, CFSTR("C"))
        || !column_selection_stall_state_observe(&state, CFSTR("C"))
        || column_selection_probe_interval_microseconds() != 50000;
    column_selection_stall_state_release(&state);
    if (failed) {
        fprintf(stderr, "edge monitor self-test: Column stall state failed\n");
        return 1;
    }
    printf("Column selection stall state tests passed.\n");
    return 0;
}

static AXUIElementRef copy_direct_column_child(
    AXUIElementRef element,
    AXUIElementRef container
) {
    AXUIElementRef current = (AXUIElementRef)CFRetain(element);
    for (int depth = 0; depth < 8; ++depth) {
        AXUIElementRef parent = copy_ax_element_attribute(
            current,
            kAXParentAttribute
        );
        if (!parent) {
            CFRelease(current);
            return NULL;
        }
        if (CFEqual(parent, container)) {
            CFRelease(parent);
            return current;
        }
        CFRelease(current);
        current = parent;
    }
    CFRelease(current);
    return NULL;
}

static AXUIElementRef copy_selected_column_child(
    AXUIElementRef container
) {
    CFArrayRef selected = copy_array_attribute(
        container,
        kAXSelectedChildrenAttribute
    );
    if (!selected || CFArrayGetCount(selected) == 0) {
        if (selected) CFRelease(selected);
        return NULL;
    }
    AXUIElementRef selected_element =
        (AXUIElementRef)CFArrayGetValueAtIndex(selected, 0);
    AXUIElementRef child = copy_direct_column_child(
        selected_element,
        container
    );
    CFRelease(selected);
    return child;
}
