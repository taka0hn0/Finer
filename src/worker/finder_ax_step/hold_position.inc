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
