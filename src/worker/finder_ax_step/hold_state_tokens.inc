static int run_hold_start(direction_t direction) {
    notify_keystroke(
        keystroke_key_code_for_direction(direction),
        0,
        keystroke_suppressed_key_code_for_direction(direction),
        0
    );
    if (direction == direction_down || direction == direction_up) {
        direction_t opposite = direction == direction_down
            ? direction_up
            : direction_down;
        if (!write_hold_token(direction)) return 1;
        if (!clear_hold_token(opposite)) {
            clear_hold_token(direction);
            return 1;
        }
        bool enqueued = enqueue_worker_step(direction);
        if (!enqueued) clear_hold_token(direction);
        return enqueued ? 0 : 1;
    }
    bool enqueued = enqueue_worker_step(direction);
    bool token_written = write_hold_token(direction);
    return enqueued && token_written ? 0 : 1;
}

static int run_hold_token_start(direction_t direction) {
    return write_hold_token(direction) ? 0 : 1;
}

static bool hold_token_matches(
    int token_fd,
    const char *initial_token,
    ssize_t initial_length
) {
    char current_token[64];
    ssize_t current_length = read_token(
        token_fd,
        current_token,
        sizeof(current_token)
    );
    return current_length == initial_length
        && memcmp(initial_token, current_token, (size_t)initial_length) == 0;
}

static bool parse_hold_token_timestamp(
    const char *token,
    ssize_t token_length,
    uint64_t *timestamp
) {
    if (!token || token_length <= 0 || !timestamp) return false;
    const char *separator = memchr(token, '\t', (size_t)token_length);
    if (!separator) return false;
    size_t timestamp_length = (size_t)(separator - token);
    if (timestamp_length == 0 || timestamp_length >= 32) return false;

    char buffer[32];
    memcpy(buffer, token, timestamp_length);
    buffer[timestamp_length] = '\0';
    char *end = NULL;
    errno = 0;
    uint64_t parsed = strtoull(buffer, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *timestamp = parsed;
    return true;
}

static ssize_t read_locked_token(
    int descriptor,
    char *buffer,
    size_t size
) {
    if (descriptor < 0 || flock(descriptor, LOCK_SH) != 0) return -1;
    ssize_t length = read_token(descriptor, buffer, size);
    flock(descriptor, LOCK_UN);
    return length;
}

static int open_vertical_token(direction_t direction) {
    char path[PATH_MAX];
    token_path(path, sizeof(path), direction);
    return open(path, O_CREAT | O_RDONLY, 0600);
}

static vertical_hold_refresh_t refresh_vertical_hold_state(
    vertical_hold_state_t *state
) {
    char down_token[64];
    char up_token[64];
    ssize_t down_length = read_locked_token(
        state->down_fd,
        down_token,
        sizeof(down_token)
    );
    ssize_t up_length = read_locked_token(
        state->up_fd,
        up_token,
        sizeof(up_token)
    );
    uint64_t down_timestamp = 0;
    uint64_t up_timestamp = 0;
    bool down_active = down_length > 0 && parse_hold_token_timestamp(
        down_token,
        down_length,
        &down_timestamp
    );
    bool up_active = up_length > 0 && parse_hold_token_timestamp(
        up_token,
        up_length,
        &up_timestamp
    );
    if (!down_active && !up_active) return vertical_hold_stopped;

    direction_t next_direction;
    const char *next_token;
    ssize_t next_length;
    if (down_active && (!up_active || down_timestamp >= up_timestamp)) {
        next_direction = direction_down;
        next_token = down_token;
        next_length = down_length;
    } else {
        next_direction = direction_up;
        next_token = up_token;
        next_length = up_length;
    }

    bool changed = state->token_length != next_length
        || state->direction != next_direction
        || memcmp(state->token, next_token, (size_t)next_length) != 0;
    state->direction = next_direction;
    state->token_length = next_length;
    memcpy(state->token, next_token, (size_t)next_length);
    return changed ? vertical_hold_changed : vertical_hold_unchanged;
}

static bool vertical_hold_state_create(vertical_hold_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->down_fd = open_vertical_token(direction_down);
    state->up_fd = open_vertical_token(direction_up);
    state->token_length = 0;
    if (state->down_fd < 0 || state->up_fd < 0) {
        if (state->down_fd >= 0) close(state->down_fd);
        if (state->up_fd >= 0) close(state->up_fd);
        state->down_fd = -1;
        state->up_fd = -1;
        return false;
    }
    return refresh_vertical_hold_state(state) != vertical_hold_stopped;
}

static void vertical_hold_state_release(vertical_hold_state_t *state) {
    if (state->down_fd >= 0) close(state->down_fd);
    if (state->up_fd >= 0) close(state->up_fd);
    state->down_fd = -1;
    state->up_fd = -1;
}

static int run_vertical_hold_state_self_test(void) {
    if (!clear_hold_token(direction_down)
        || !clear_hold_token(direction_up)
        || !write_hold_token(direction_down)) {
        return 1;
    }

    vertical_hold_state_t state;
    if (!vertical_hold_state_create(&state)
        || state.direction != direction_down) {
        return 1;
    }
    if (refresh_vertical_hold_state(&state) != vertical_hold_unchanged) {
        vertical_hold_state_release(&state);
        return 1;
    }

    if (!write_hold_token(direction_up)
        || refresh_vertical_hold_state(&state) != vertical_hold_changed
        || state.direction != direction_up) {
        vertical_hold_state_release(&state);
        return 1;
    }
    if (!write_hold_token(direction_down)
        || refresh_vertical_hold_state(&state) != vertical_hold_changed
        || state.direction != direction_down) {
        vertical_hold_state_release(&state);
        return 1;
    }
    if (!clear_hold_token(direction_down)
        || !clear_hold_token(direction_up)
        || refresh_vertical_hold_state(&state) != vertical_hold_stopped) {
        vertical_hold_state_release(&state);
        return 1;
    }
    vertical_hold_state_release(&state);
    return 0;
}
