static int run_hold_repeat(direction_t direction) {
    bool vertical = direction == direction_down || direction == direction_up;
    int controller_lock_fd = -1;
    vertical_hold_state_t hold_state;
    hold_state.down_fd = -1;
    hold_state.up_fd = -1;
    int token_fd = -1;
    char initial_token[64];
    ssize_t initial_length = 0;

    if (vertical) {
        controller_lock_fd = open_vertical_hold_controller_lock();
        if (controller_lock_fd < 0) return 1;
        if (flock(controller_lock_fd, LOCK_EX | LOCK_NB) != 0) {
            close(controller_lock_fd);
            return errno == EWOULDBLOCK ? 0 : 1;
        }
        if (!vertical_hold_state_create(&hold_state)) {
            flock(controller_lock_fd, LOCK_UN);
            close(controller_lock_fd);
            return 0;
        }
        direction = hold_state.direction;
    } else {
        char path[PATH_MAX];
        token_path(path, sizeof(path), direction);
        token_fd = open(path, O_RDONLY);
        if (token_fd < 0) return 0;
        initial_length = read_token(
            token_fd,
            initial_token,
            sizeof(initial_token)
        );
        if (initial_length <= 0) {
            close(token_fd);
            return 0;
        }
    }

    navigation_context_t context;
    int lock_fd = open_movement_lock();
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
    bool context_created = navigation_context_create(&context);
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    if (!context_created) {
        if (lock_fd >= 0) close(lock_fd);
        if (vertical) {
            vertical_hold_state_release(&hold_state);
            flock(controller_lock_fd, LOCK_UN);
            close(controller_lock_fd);
        } else {
            close(token_fd);
        }
        return 1;
    }

    CFIndex last_position = 0;
    double deadline = monotonic_seconds() + 300.0;
    if (supports_native_list_hold(&context, direction)) {
        last_position = run_native_list_hold_repeat(
            &context,
            &hold_state,
            deadline,
            lock_fd
        );
    } else if (supports_fast_ax_hold(&context, direction)) {
        last_position = run_fast_ax_hold_repeat(
            &context,
            &hold_state,
            deadline,
            lock_fd
        );
    } else {
        unsigned iteration = 0;
        bool use_navigation_anchor = true;
        while (monotonic_seconds() < deadline) {
            if (vertical) {
                if (refresh_vertical_hold_state(&hold_state)
                        == vertical_hold_stopped) {
                    break;
                }
                direction = hold_state.direction;
            } else {
                if (!hold_token_matches(
                        token_fd,
                        initial_token,
                        initial_length
                    )) {
                    break;
                }
            }
            if ((iteration++ % 20) == 0
                && !finder_is_frontmost(NULL, NULL)) {
                break;
            }

            last_position = worker_move(
                &context,
                &context_created,
                direction,
                lock_fd,
                use_navigation_anchor,
                NULL
            );
            use_navigation_anchor = false;
            if (last_position == 0) break;
            usleep(5000);
        }
    }

    if (last_position > 0 && context_created && context.has_marks
        && !write_navigation_anchor(&context)) {
        last_position = 0;
    }
    if (lock_fd >= 0) close(lock_fd);
    if (context_created) navigation_context_release(&context);
    if (vertical) {
        vertical_hold_state_release(&hold_state);
        flock(controller_lock_fd, LOCK_UN);
        close(controller_lock_fd);
    } else {
        close(token_fd);
    }
    if (last_position > 0) printf("%ld\n", last_position);
    return 0;
}

typedef struct {
    CFIndex previous;
    unsigned stable_observations;
} vertical_edge_monitor_state_t;
