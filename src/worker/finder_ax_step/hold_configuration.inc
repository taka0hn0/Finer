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

// Fixed-cadence tick source for the hold and edge-monitor loops. Each loop
// keeps its own probe schedule and stop conditions; only the tick arithmetic is
// shared, so nothing here reaches for Finder state or a callback.
static void hold_pacer_start(
    hold_pacer_t *pacer,
    useconds_t interval_microseconds
) {
    pacer->interval_ticks = mach_ticks_for_microseconds(interval_microseconds);
    pacer->next_step = mach_absolute_time();
}

static void hold_pacer_wait(const hold_pacer_t *pacer) {
    if (mach_absolute_time() < pacer->next_step) {
        mach_wait_until(pacer->next_step);
    }
}

// Skips whole intervals that already elapsed so a slow step cannot leave a
// backlog of catch-up taps for Finder to process after the key is released.
// Returns the sampled tick count so a caller with its own probe schedule can
// reuse it instead of reading the clock twice.
static uint64_t hold_pacer_advance(hold_pacer_t *pacer) {
    pacer->next_step += pacer->interval_ticks;
    uint64_t finished = mach_absolute_time();
    if (finished >= pacer->next_step) {
        uint64_t missed =
            (finished - pacer->next_step) / pacer->interval_ticks + 1;
        pacer->next_step += missed * pacer->interval_ticks;
    }
    return finished;
}

// Synchronous AX steps already finish selection and reveal before returning.
// Enforce the minimum start-to-start interval, without waiting for another
// timer slot after a slow step or replaying any missed slots.
static void hold_pacer_complete_synchronous_step(
    hold_pacer_t *pacer,
    uint64_t step_started
) {
    pacer->next_step = step_started + pacer->interval_ticks;
}

// Restarts the cadence from now, for the Column wrap that just moved the
// selection to the opposite edge.
static void hold_pacer_restart(hold_pacer_t *pacer) {
    pacer->next_step = mach_absolute_time() + pacer->interval_ticks;
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

static bool supports_native_vertical_hold(
    const navigation_context_t *context,
    direction_t direction
) {
    const char *column_native = getenv("FINDER_VIM_HOLD_COLUMN_NATIVE");
    bool supported_view = context->role == navigation_outline
        ? native_list_hold_enabled()
        : context->role == navigation_list
            && column_native && strcmp(column_native, "1") == 0;
    return supported_view
        && mark_state_is_empty()
        && (direction == direction_down || direction == direction_up);
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
