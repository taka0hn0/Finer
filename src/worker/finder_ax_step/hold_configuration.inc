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
