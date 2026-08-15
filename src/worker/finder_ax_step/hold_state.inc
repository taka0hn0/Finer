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

typedef enum {
    vertical_hold_stopped,
    vertical_hold_unchanged,
    vertical_hold_changed,
} vertical_hold_refresh_t;

typedef struct {
    int down_fd;
    int up_fd;
    direction_t direction;
    char token[64];
    ssize_t token_length;
} vertical_hold_state_t;

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

static bool fast_ax_hold_mode_enabled(void) {
    const char *mode = getenv("FINDER_VIM_HOLD_MODE");
    return !mode || strcmp(mode, "ax-fast") == 0;
}

static useconds_t fast_ax_hold_interval_microseconds(void) {
    const useconds_t default_interval = 8333;
    const char *value = getenv("FINDER_VIM_HOLD_INTERVAL_US");
    if (!value || value[0] == '\0') return default_interval;

    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || end[0] != '\0'
        || parsed < 4000 || parsed > 50000) {
        return default_interval;
    }
    return (useconds_t)parsed;
}

static useconds_t column_owned_hold_interval_microseconds(void) {
    const char *override = getenv("FINDER_VIM_HOLD_INTERVAL_US");
    if (override && override[0] != '\0') {
        return fast_ax_hold_interval_microseconds();
    }
    return 8333;
}

static useconds_t column_selection_probe_interval_microseconds(void) {
    return 50000;
}

static useconds_t list_native_hold_interval_microseconds(void) {
    const char *override = getenv("FINDER_VIM_HOLD_INTERVAL_US");
    if (override && override[0] != '\0') {
        return fast_ax_hold_interval_microseconds();
    }
    return 16667;
}

static uint64_t mach_ticks_for_microseconds(useconds_t microseconds) {
    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase) != KERN_SUCCESS
        || timebase.numer == 0 || timebase.denom == 0) {
        return (uint64_t)microseconds * 1000ULL;
    }
    long double nanoseconds = (long double)microseconds * 1000.0L;
    return (uint64_t)(
        nanoseconds
            * (long double)timebase.denom
            / (long double)timebase.numer
    );
}

static bool mark_state_is_empty(void) {
    CFArrayRef urls = copy_mark_urls();
    bool empty = CFArrayGetCount(urls) == 0;
    CFRelease(urls);
    return empty;
}

static bool native_list_hold_enabled(void) {
    const char *value = getenv("FINDER_VIM_HOLD_LIST_NATIVE");
    return !value || strcmp(value, "0") != 0;
}

static bool supports_native_list_hold(
    const navigation_context_t *context,
    direction_t direction
) {
    return native_list_hold_enabled()
        && mark_state_is_empty()
        && (direction == direction_down || direction == direction_up)
        && context->role == navigation_outline;
}

static bool supports_fast_ax_hold(
    const navigation_context_t *context,
    direction_t direction
) {
    bool vertical_list = (direction == direction_down
            || direction == direction_up)
        && (context->role == navigation_outline
            || context->role == navigation_list);
    return fast_ax_hold_mode_enabled()
        && mark_state_is_empty()
        && vertical_list;
}

static bool direction_advances_index(direction_t direction) {
    return direction == direction_down || direction == direction_right;
}

static CFIndex fast_selected_index(
    const navigation_context_t *context
) {
    CFIndex item_count = CFArrayGetCount(context->items);
    CFStringRef selected_attribute = context->role == navigation_outline
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute;
    CFArrayRef selected = copy_array_attribute(
        context->container,
        selected_attribute
    );
    if (!selected || CFArrayGetCount(selected) == 0) {
        if (selected) CFRelease(selected);
        return -1;
    }

    AXUIElementRef selected_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        selected,
        0
    );
    CFIndex index = CFArrayGetFirstIndexOfValue(
        context->items,
        CFRangeMake(0, item_count),
        selected_item
    );
    if (index == kCFNotFound && context->role != navigation_outline) {
        AXUIElementRef item_ancestor = copy_navigation_item_ancestor(
            selected_item,
            context->items
        );
        if (item_ancestor) {
            index = CFArrayGetFirstIndexOfValue(
                context->items,
                CFRangeMake(0, item_count),
                item_ancestor
            );
            CFRelease(item_ancestor);
        }
    }
    CFRelease(selected);
    return index == kCFNotFound ? -1 : index;
}

static CFIndex wait_for_fast_ax_index(
    const navigation_context_t *context,
    CFIndex expected
) {
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        CFIndex current = fast_selected_index(context);
        if (current == expected) return current + 1;
        usleep(500);
    }
    return 0;
}

static CFIndex recover_fast_ax_hold_position(
    const navigation_context_t *context,
    CFIndex predicted,
    direction_t direction
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex candidate = predicted;
    CFIndex attempt_limit = count < 8 ? count : 8;
    for (CFIndex attempt = 0; attempt < attempt_limit; ++attempt) {
        if (select_index(context, candidate)) {
            CFIndex position = wait_for_fast_ax_index(context, candidate);
            if (position > 0) return position;
        }
        candidate = direction_advances_index(direction)
            ? (candidate + 1) % count
            : (candidate + count - 1) % count;
    }
    return 0;
}

static CFIndex settle_fast_ax_hold_position(
    const navigation_context_t *context,
    CFIndex predicted,
    direction_t direction
) {
    CFIndex previous = -1;
    unsigned stable_samples = 0;
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        CFIndex current = fast_selected_index(context);
        if (current >= 0 && current == previous) {
            if (++stable_samples >= 2) return current + 1;
        } else {
            previous = current;
            stable_samples = 0;
        }
        usleep(1000);
    }
    if (previous >= 0) return previous + 1;

    // Group headers in Finder's outline can accept AXSelectedRows writes while
    // leaving the real selection empty. Keep that verification out of the hot
    // loop, then recover from the predicted row only when the hold stops.
    return recover_fast_ax_hold_position(context, predicted, direction);
}

static bool fast_ax_hold_scroll_to_visible_enabled(void) {
    const char *value = getenv("FINDER_VIM_HOLD_SCROLL_TO_VISIBLE");
    return !value || strcmp(value, "0") != 0;
}

typedef struct {
    AXUIElementRef scroll_bar;
    AXUIElementRef value_indicator;
    AXUIElementRef increment_button;
    AXUIElementRef decrement_button;
    CFNumberRef minimum_value;
    CFNumberRef maximum_value;
    CFIndex first_visible;
    CFIndex last_visible;
    bool enabled;
} fast_ax_scroll_state_t;
