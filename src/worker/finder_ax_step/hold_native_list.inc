static CFIndex run_native_list_hold_repeat(
    navigation_context_t *context,
    vertical_hold_state_t *hold_state,
    double deadline,
    int lock_fd
) {
    CFIndex last_observed = current_index(context);
    if (last_observed < 0) return 0;
    CFIndex first = selectable_vertical_edge_index(context, false);
    CFIndex last = selectable_vertical_edge_index(context, true);
    if (first < 0 || last < first) return 0;

    useconds_t interval = list_native_hold_interval_microseconds();
    uint64_t interval_ticks = mach_ticks_for_microseconds(interval);
    uint64_t next_step = mach_absolute_time();
    uint64_t probe_interval_ticks = mach_ticks_for_microseconds(16667);
    uint64_t next_probe = next_step + probe_interval_ticks;
    direction_t direction = hold_state->direction;
    CGKeyCode key_code = arrow_key_code(direction);

    while (monotonic_seconds() < deadline) {
        uint64_t now = mach_absolute_time();
        if (now < next_step) {
            mach_wait_until(next_step);
        }
        vertical_hold_refresh_t refresh = refresh_vertical_hold_state(
            hold_state
        );
        if (monotonic_seconds() >= deadline
            || refresh == vertical_hold_stopped
            || !process_is_frontmost(context->finder_pid)) {
            break;
        }
        if (hold_state->direction != direction) {
            direction = hold_state->direction;
            key_code = arrow_key_code(direction);
            CFIndex observed = current_index(context);
            if (observed >= 0) last_observed = observed;
            next_probe = mach_absolute_time() + probe_interval_ticks;
        }

        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        bool posted = post_key_code(key_code);
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        if (!posted) break;

        next_step += interval_ticks;
        uint64_t finished = mach_absolute_time();
        if (finished >= next_step) {
            uint64_t missed = (finished - next_step) / interval_ticks + 1;
            next_step += missed * interval_ticks;
        }

        if (finished >= next_probe) {
            if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
            CFIndex observed = current_index(context);
            if (observed >= 0) last_observed = observed;
            bool at_edge = observed >= 0
                && (direction == direction_down
                    ? observed == last
                    : observed == first);
            vertical_hold_refresh_t edge_refresh =
                refresh_vertical_hold_state(hold_state);
            if (at_edge
                && edge_refresh == vertical_hold_unchanged
                && hold_state->direction == direction
                && process_is_frontmost(context->finder_pid)
                && post_vertical_wrap_chord(
                    context->finder_pid,
                    direction
                )) {
                last_observed = direction == direction_down
                    ? first
                    : last;
            }
            if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
            do {
                next_probe += probe_interval_ticks;
            } while (next_probe <= finished);
        }
    }

    if (!process_is_frontmost(context->finder_pid)) return 0;

    // A complete tap has no held key state, but Finder may still be consuming
    // a few already-posted taps. Hold the movement lock until the actual
    // selection has remained unchanged longer than one tap interval so a
    // reversal cannot overlap that tail.
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
    uint64_t settle_started = mach_absolute_time();
    uint64_t last_change = settle_started;
    uint64_t settle_limit = settle_started
        + mach_ticks_for_microseconds(250000);
    uint64_t stable_ticks = mach_ticks_for_microseconds(40000);
    while (mach_absolute_time() < settle_limit) {
        CFIndex observed = current_index(context);
        uint64_t now = mach_absolute_time();
        if (observed >= 0 && observed != last_observed) {
            last_observed = observed;
            last_change = now;
        }
        if (now - last_change >= stable_ticks) break;
        usleep(2000);
    }
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    return last_observed + 1;
}
