// Vertical holds and horizontal holds prove they are still wanted in different
// ways: `j/k` share one controller lock plus the down/up token pair, while
// `h/l` watch a single direction token. Both are opened and released here so
// the repeat loop below has one exit path.
typedef enum {
    hold_session_ready,
    hold_session_idle,
    hold_session_failed,
} hold_session_status_t;

typedef struct {
    bool vertical;
    int controller_lock_fd;
    int token_fd;
    vertical_hold_state_t hold_state;
    char initial_token[64];
    ssize_t initial_token_length;
} hold_session_t;

static void hold_session_end(hold_session_t *session) {
    if (session->vertical) {
        vertical_hold_state_release(&session->hold_state);
        if (session->controller_lock_fd >= 0) {
            flock(session->controller_lock_fd, LOCK_UN);
            close(session->controller_lock_fd);
            session->controller_lock_fd = -1;
        }
        return;
    }
    if (session->token_fd >= 0) {
        close(session->token_fd);
        session->token_fd = -1;
    }
}

static hold_session_status_t hold_session_begin(
    hold_session_t *session,
    direction_t *direction
) {
    memset(session, 0, sizeof(*session));
    session->controller_lock_fd = -1;
    session->token_fd = -1;
    session->hold_state.down_fd = -1;
    session->hold_state.up_fd = -1;
    session->vertical = *direction == direction_down
        || *direction == direction_up;

    if (!session->vertical) {
        char path[PATH_MAX];
        token_path(path, sizeof(path), *direction);
        session->token_fd = open(path, O_RDONLY);
        if (session->token_fd < 0) return hold_session_idle;
        session->initial_token_length = read_token(
            session->token_fd,
            session->initial_token,
            sizeof(session->initial_token)
        );
        if (session->initial_token_length <= 0) {
            hold_session_end(session);
            return hold_session_idle;
        }
        return hold_session_ready;
    }

    session->controller_lock_fd = open_vertical_hold_controller_lock();
    if (session->controller_lock_fd < 0) return hold_session_failed;
    if (flock(session->controller_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        int lock_error = errno;
        close(session->controller_lock_fd);
        session->controller_lock_fd = -1;
        return lock_error == EWOULDBLOCK
            ? hold_session_idle
            : hold_session_failed;
    }
    if (!vertical_hold_state_create(&session->hold_state)) {
        hold_session_end(session);
        return hold_session_idle;
    }
    // A reversal may already have handed ownership to the other direction.
    *direction = session->hold_state.direction;
    return hold_session_ready;
}

// Fallback repeat for the cases the specialised loops do not cover: horizontal
// holds, Column, and any vertical hold with confirmed marks.
static CFIndex run_worker_hold_repeat(
    navigation_context_t *context,
    bool *context_created,
    hold_session_t *session,
    direction_t direction,
    double deadline,
    int lock_fd
) {
    CFIndex last_position = 0;
    unsigned iteration = 0;
    bool use_navigation_anchor = true;
    while (monotonic_seconds() < deadline) {
        if (session->vertical) {
            if (refresh_vertical_hold_state(&session->hold_state)
                    == vertical_hold_stopped) {
                break;
            }
            direction = session->hold_state.direction;
        } else if (!hold_token_matches(
                session->token_fd,
                session->initial_token,
                session->initial_token_length
            )) {
            break;
        }
        if ((iteration++ % 20) == 0 && !finder_is_frontmost(NULL, NULL)) {
            break;
        }

        last_position = worker_move(
            context,
            context_created,
            direction,
            lock_fd,
            use_navigation_anchor,
            NULL
        );
        use_navigation_anchor = false;
        if (last_position == 0) break;
        usleep(5000);
    }
    return last_position;
}

static int run_hold_repeat(direction_t direction) {
    hold_session_t session;
    switch (hold_session_begin(&session, &direction)) {
        case hold_session_idle: return 0;
        case hold_session_failed: return 1;
        case hold_session_ready: break;
    }

    navigation_context_t context;
    int lock_fd = open_movement_lock();
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
    bool context_created = navigation_context_create(&context);
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    if (!context_created) {
        if (lock_fd >= 0) close(lock_fd);
        hold_session_end(&session);
        return 1;
    }

    CFIndex last_position = 0;
    double deadline = monotonic_seconds() + 300.0;
    if (supports_native_list_hold(&context, direction)) {
        last_position = run_native_list_hold_repeat(
            &context,
            &session.hold_state,
            deadline,
            lock_fd
        );
    } else if (supports_fast_ax_hold(&context, direction)) {
        last_position = run_fast_ax_hold_repeat(
            &context,
            &session.hold_state,
            deadline,
            lock_fd
        );
    } else {
        last_position = run_worker_hold_repeat(
            &context,
            &context_created,
            &session,
            direction,
            deadline,
            lock_fd
        );
    }

    if (last_position > 0 && context_created && context.has_marks
        && !write_navigation_anchor(&context)) {
        last_position = 0;
    }
    if (lock_fd >= 0) close(lock_fd);
    if (context_created) navigation_context_release(&context);
    hold_session_end(&session);
    if (last_position > 0) printf("%ld\n", last_position);
    return 0;
}
