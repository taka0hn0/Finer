static bool direction_advances_index(direction_t direction) {
    return direction == direction_down || direction == direction_right;
}

static CFIndex wait_for_fast_ax_index(
    const navigation_context_t *context,
    CFIndex expected
) {
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        CFIndex current = selected_index_fast(context);
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
        CFIndex current = selected_index_fast(context);
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
