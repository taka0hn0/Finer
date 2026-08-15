static void fast_ax_scroll_state_create(
    const navigation_context_t *context,
    CFIndex current,
    fast_ax_scroll_state_t *state
) {
    memset(state, 0, sizeof(*state));
    state->first_visible = -1;
    state->last_visible = -1;
    state->enabled = fast_ax_hold_scroll_to_visible_enabled();
    if (!state->enabled) return;

    CFStringRef visible_attribute = context->role == navigation_outline
        ? kAXVisibleRowsAttribute
        : kAXVisibleChildrenAttribute;
    CFArrayRef visible = copy_array_attribute(
        context->container,
        visible_attribute
    );
    if (visible && CFArrayGetCount(visible) > 0) {
        CFIndex item_count = CFArrayGetCount(context->items);
        CFIndex visible_count = CFArrayGetCount(visible);
        if (context->role == navigation_outline
            && current >= 0 && current < item_count) {
            AXUIElementRef selected_item =
                (AXUIElementRef)CFArrayGetValueAtIndex(
                    context->items,
                    current
                );
            CFStringRef selected_url = copy_navigation_item_url_string(
                selected_item,
                0
            );
            if (selected_url) {
                for (CFIndex visible_index = 0;
                        visible_index < visible_count;
                        ++visible_index) {
                    AXUIElementRef visible_item =
                        (AXUIElementRef)CFArrayGetValueAtIndex(
                            visible,
                            visible_index
                        );
                    if (!navigation_item_matches_url(
                            visible_item,
                            selected_url
                        )) {
                        continue;
                    }
                    state->first_visible = current - visible_index;
                    state->last_visible =
                        state->first_visible + visible_count - 1;
                    if (state->first_visible < 0) {
                        state->last_visible -= state->first_visible;
                        state->first_visible = 0;
                    }
                    if (state->last_visible >= item_count) {
                        CFIndex overflow =
                            state->last_visible - item_count + 1;
                        state->first_visible -= overflow;
                        state->last_visible = item_count - 1;
                    }
                    break;
                }
                CFRelease(selected_url);
            }
        } else {
            state->first_visible = CFArrayGetFirstIndexOfValue(
                context->items,
                CFRangeMake(0, item_count),
                CFArrayGetValueAtIndex(visible, 0)
            );
            state->last_visible = CFArrayGetFirstIndexOfValue(
                context->items,
                CFRangeMake(0, item_count),
                CFArrayGetValueAtIndex(visible, visible_count - 1)
            );
        }
        if (state->first_visible == kCFNotFound
            || state->last_visible == kCFNotFound) {
            state->first_visible = -1;
            state->last_visible = -1;
        }
    }
    if (visible) CFRelease(visible);

    if (context->role != navigation_outline) return;
    AXUIElementRef parent = copy_ax_element_attribute(
        context->container,
        kAXParentAttribute
    );
    if (!parent) return;
    state->scroll_bar = copy_ax_element_attribute(
        parent,
        kAXVerticalScrollBarAttribute
    );
    CFRelease(parent);
    if (!state->scroll_bar) return;
    state->value_indicator = copy_scroll_bar_value_indicator(
        state->scroll_bar
    );
    state->increment_button = copy_ax_element_attribute(
        state->scroll_bar,
        kAXIncrementButtonAttribute
    );
    state->decrement_button = copy_ax_element_attribute(
        state->scroll_bar,
        kAXDecrementButtonAttribute
    );
    if (state->value_indicator) {
        float minimum = 0.0f;
        float maximum = 1.0f;
        state->minimum_value = CFNumberCreate(
            kCFAllocatorDefault,
            kCFNumberFloatType,
            &minimum
        );
        state->maximum_value = CFNumberCreate(
            kCFAllocatorDefault,
            kCFNumberFloatType,
            &maximum
        );
    } else {
        double minimum = 0.0;
        double maximum = 1.0;
        state->minimum_value = CFNumberCreate(
            kCFAllocatorDefault,
            kCFNumberDoubleType,
            &minimum
        );
        state->maximum_value = CFNumberCreate(
            kCFAllocatorDefault,
            kCFNumberDoubleType,
            &maximum
        );
    }
}

static void fast_ax_scroll_state_release(fast_ax_scroll_state_t *state) {
    if (state->maximum_value) CFRelease(state->maximum_value);
    if (state->minimum_value) CFRelease(state->minimum_value);
    if (state->decrement_button) CFRelease(state->decrement_button);
    if (state->increment_button) CFRelease(state->increment_button);
    if (state->value_indicator) CFRelease(state->value_indicator);
    if (state->scroll_bar) CFRelease(state->scroll_bar);
    memset(state, 0, sizeof(*state));
}

static void scroll_fast_ax_target_to_visible(
    const navigation_context_t *context,
    fast_ax_scroll_state_t *state,
    CFIndex current,
    CFIndex target,
    direction_t direction,
    AXUIElementRef item
) {
    if (!state->enabled) return;

    CFIndex item_count = CFArrayGetCount(context->items);
    CFIndex visible_span = state->first_visible >= 0
        && state->last_visible >= state->first_visible
        ? state->last_visible - state->first_visible
        : 0;
    bool advances = direction_advances_index(direction);
    bool wrapped = (advances && target < current)
        || (!advances && target > current);
    if (wrapped && state->scroll_bar) {
        CFNumberRef edge = direction == direction_down
            ? state->minimum_value
            : state->maximum_value;
        AXUIElementRef scroll_target =
            context->role == navigation_outline && state->value_indicator
                ? state->value_indicator
                : state->scroll_bar;
        if (!edge || set_attribute(
                scroll_target,
                kAXValueAttribute,
                edge
            ) != kAXErrorSuccess) {
            perform_ax_action(item, CFSTR("AXScrollToVisible"));
            return;
        }
        if (direction == direction_down) {
            state->first_visible = 0;
            state->last_visible = visible_span;
        } else {
            state->last_visible = item_count - 1;
            state->first_visible = state->last_visible - visible_span;
        }
        return;
    }

    bool below = state->last_visible >= 0 && target > state->last_visible;
    bool above = state->first_visible >= 0 && target < state->first_visible;
    if (state->first_visible < 0 || state->last_visible < 0) {
        perform_ax_action(item, CFSTR("AXScrollToVisible"));
        return;
    }
    if (!below && !above) return;

    bool scrolled = false;
    if (context->role == navigation_outline) {
        CFIndex distance = below
            ? target - state->last_visible
            : state->first_visible - target;
        CFIndex next_first = below
            ? state->first_visible + distance
            : target;
        CFIndex maximum_first = item_count - visible_span - 1;
        if (state->value_indicator && maximum_first > 0) {
            float scroll_value = (float)next_first
                / (float)maximum_first;
            CFNumberRef value = CFNumberCreate(
                kCFAllocatorDefault,
                kCFNumberFloatType,
                &scroll_value
            );
            scrolled = value && set_attribute(
                state->value_indicator,
                kAXValueAttribute,
                value
            ) == kAXErrorSuccess;
            if (value) CFRelease(value);
        }
        AXUIElementRef button = below
            ? state->increment_button
            : state->decrement_button;
        if (!scrolled && button) {
            scrolled = true;
            for (CFIndex step = 0; step < distance; ++step) {
                if (perform_ax_action(button, kAXPressAction)
                        != kAXErrorSuccess) {
                    scrolled = false;
                    break;
                }
            }
        }
    } else {
        scrolled = perform_ax_action(item, CFSTR("AXScrollToVisible"))
            == kAXErrorSuccess;
    }
    if (!scrolled) return;

    if (below) {
        CFIndex distance = target - state->last_visible;
        state->first_visible += distance;
        state->last_visible = target;
    } else {
        CFIndex distance = state->first_visible - target;
        state->first_visible = target;
        state->last_visible -= distance;
    }
}

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
