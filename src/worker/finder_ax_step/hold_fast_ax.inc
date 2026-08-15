static CFIndex select_fast_ax_next(
    const navigation_context_t *context,
    CFIndex current,
    direction_t direction,
    CFMutableArrayRef selection,
    fast_ax_scroll_state_t *scroll_state
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex target = current;
    for (CFIndex attempt = 0; attempt < count; ++attempt) {
        target = direction_advances_index(direction)
            ? (target + 1) % count
            : (target + count - 1) % count;
        bool wrapped = (direction_advances_index(direction)
                && target < current)
            || (!direction_advances_index(direction)
                && target > current);
        if (context->role == navigation_outline && wrapped
            && post_vertical_wrap_chord(
                context->finder_pid,
                direction
            )) {
            CFIndex edge = selectable_vertical_edge_index(
                context,
                direction == direction_up
            );
            if (edge < 0) return -1;
            CFIndex visible_span = scroll_state->first_visible >= 0
                && scroll_state->last_visible
                    >= scroll_state->first_visible
                ? scroll_state->last_visible
                    - scroll_state->first_visible
                : 0;
            if (direction == direction_down) {
                scroll_state->first_visible = 0;
                scroll_state->last_visible = visible_span;
            } else {
                scroll_state->last_visible = count - 1;
                scroll_state->first_visible =
                    scroll_state->last_visible - visible_span;
            }
            return edge;
        }
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            target
        );
        if (!navigation_item_is_selectable(context, target)) continue;
        if (CFArrayGetCount(selection) == 0) {
            CFArrayAppendValue(selection, item);
        } else {
            CFArraySetValueAtIndex(selection, 0, item);
        }

        AXError error;
        if (context->role == navigation_outline) {
            error = set_attribute(
                context->container,
                kAXSelectedRowsAttribute,
                selection
            );
            if (error != kAXErrorSuccess) {
                error = set_attribute(
                    item,
                    kAXSelectedAttribute,
                    kCFBooleanTrue
                );
            }
        } else {
            error = set_attribute(
                context->container,
                kAXSelectedChildrenAttribute,
                selection
            );
        }
        if (error == kAXErrorSuccess) {
            scroll_fast_ax_target_to_visible(
                context,
                scroll_state,
                current,
                target,
                direction,
                item
            );
            return target;
        }
    }
    return -1;
}

static CFIndex run_fast_ax_hold_repeat(
    navigation_context_t *context,
    vertical_hold_state_t *hold_state,
    double deadline,
    int lock_fd
) {
    CFIndex current = current_index(context);
    if (current < 0) return 0;

    CFMutableArrayRef selection = CFArrayCreateMutable(
        kCFAllocatorDefault,
        1,
        &kCFTypeArrayCallBacks
    );
    if (!selection) return 0;
    fast_ax_scroll_state_t scroll_state;
    fast_ax_scroll_state_create(context, current, &scroll_state);

    useconds_t interval = fast_ax_hold_interval_microseconds();
    uint64_t interval_ticks = mach_ticks_for_microseconds(interval);
    uint64_t next_step = mach_absolute_time();
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
        direction_t direction = hold_state->direction;
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        CFIndex target = select_fast_ax_next(
            context,
            current,
            direction,
            selection,
            &scroll_state
        );
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        if (target < 0) break;
        current = target;

        next_step += interval_ticks;
        uint64_t finished = mach_absolute_time();
        if (finished >= next_step) {
            uint64_t missed = (finished - next_step) / interval_ticks + 1;
            next_step += missed * interval_ticks;
        }
    }

    CFIndex settled = 0;
    if (process_is_frontmost(context->finder_pid)) {
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        settled = settle_fast_ax_hold_position(
            context,
            current,
            hold_state->direction
        );
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    }
    fast_ax_scroll_state_release(&scroll_state);
    CFRelease(selection);
    return settled;
}
