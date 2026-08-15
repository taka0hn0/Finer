static bool wait_for_vertical_edge_delay(
    const navigation_context_t *context,
    direction_t direction,
    uint64_t remaining_microseconds,
    double deadline
) {
    while (remaining_microseconds > 0) {
        if (!arrow_key_is_down(direction)
            || !process_is_frontmost(context->finder_pid)) {
            return false;
        }

        double deadline_seconds = deadline - monotonic_seconds();
        if (deadline_seconds <= 0) return false;
        uint64_t deadline_microseconds = (uint64_t)(
            deadline_seconds * 1000000.0
        );
        uint64_t chunk = remaining_microseconds;
        if (chunk > 50000) chunk = 50000;
        if (chunk > deadline_microseconds) chunk = deadline_microseconds;
        if (chunk == 0) return false;
        usleep((useconds_t)chunk);
        remaining_microseconds -= chunk;
    }
    return true;
}

static bool wait_for_vertical_edge_probe(
    const navigation_context_t *context,
    direction_t direction,
    CFIndex current,
    CFIndex boundary,
    bool viewport_near_boundary,
    bool boundary_mismatch,
    double deadline
) {
    uint64_t delay_microseconds = boundary_mismatch
        ? 50000
        : viewport_near_boundary
            ? 8333
            : vertical_edge_probe_delay_microseconds(current, boundary);
    return wait_for_vertical_edge_delay(
        context,
        direction,
        delay_microseconds,
        deadline
    );
}

static bool vertical_viewport_near_boundary(
    const navigation_context_t *context,
    direction_t direction,
    bool *available
) {
    CFStringRef visible_attribute = context->role == navigation_outline
        ? kAXVisibleRowsAttribute
        : kAXVisibleChildrenAttribute;
    CFArrayRef visible = copy_array_attribute(
        context->container,
        visible_attribute
    );
    if (visible) {
        CFIndex item_count = CFArrayGetCount(context->items);
        CFIndex visible_count = CFArrayGetCount(visible);
        CFRelease(visible);
        if (item_count > 0 && visible_count >= item_count) {
            // Finder leaves a non-scrollable bar at value 0.0 when every item
            // fits in the viewport.  In that case either document boundary is
            // also a visible viewport boundary, so the selected edge item is
            // sufficient corroboration for wrapping.
            *available = true;
            return true;
        }
    }

    double value = 0;
    *available = vertical_scroll_value(context, &value);
    if (!*available) return false;
    return direction == direction_down ? value >= 0.98 : value <= 0.02;
}

typedef struct {
    AXUIElementRef container;
    pid_t finder_pid;
} column_edge_context_t;

typedef struct {
    CFTypeRef previous;
} column_selection_stall_state_t;
