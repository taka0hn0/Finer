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
            if (error != kAXErrorSuccess
                && column_selection_matches_index(context, target)) {
                error = kAXErrorSuccess;
            }
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
        // A failed Column write is not an unselectable heading. Continuing
        // would skip real files, possibly the entire column, in a single tick.
        if (context->role == navigation_list) return -1;
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

    hold_pacer_t pacer;
    hold_pacer_start(&pacer, fast_ax_hold_interval_microseconds());
    while (monotonic_seconds() < deadline) {
        hold_pacer_wait(&pacer);
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
        uint64_t step_started = mach_absolute_time();
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

        hold_pacer_complete_synchronous_step(&pacer, step_started);
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
