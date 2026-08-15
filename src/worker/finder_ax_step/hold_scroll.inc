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
