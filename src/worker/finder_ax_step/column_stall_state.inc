static void column_selection_stall_state_create(
    column_selection_stall_state_t *state
) {
    memset(state, 0, sizeof(*state));
}

static void column_selection_stall_state_release(
    column_selection_stall_state_t *state
) {
    if (state->previous) CFRelease(state->previous);
    memset(state, 0, sizeof(*state));
}

static bool column_selection_stall_state_observe(
    column_selection_stall_state_t *state,
    CFTypeRef current
) {
    if (!current) return false;
    bool stalled = state->previous && CFEqual(state->previous, current);
    if (!stalled) {
        if (state->previous) CFRelease(state->previous);
        state->previous = CFRetain(current);
    }
    return stalled;
}

static int run_column_selection_stall_state_self_test(void) {
    column_selection_stall_state_t state;
    column_selection_stall_state_create(&state);
    bool failed = column_selection_stall_state_observe(&state, CFSTR("A"))
        || column_selection_stall_state_observe(&state, CFSTR("B"))
        || !column_selection_stall_state_observe(&state, CFSTR("B"))
        || column_selection_stall_state_observe(&state, CFSTR("C"))
        || !column_selection_stall_state_observe(&state, CFSTR("C"))
        || column_selection_probe_interval_microseconds() != 50000;
    column_selection_stall_state_release(&state);
    if (failed) {
        fprintf(stderr, "edge monitor self-test: Column stall state failed\n");
        return 1;
    }
    printf("Column selection stall state tests passed.\n");
    return 0;
}

static AXUIElementRef copy_direct_column_child(
    AXUIElementRef element,
    AXUIElementRef container
) {
    AXUIElementRef current = (AXUIElementRef)CFRetain(element);
    for (int depth = 0; depth < 8; ++depth) {
        AXUIElementRef parent = copy_ax_element_attribute(
            current,
            kAXParentAttribute
        );
        if (!parent) {
            CFRelease(current);
            return NULL;
        }
        if (CFEqual(parent, container)) {
            CFRelease(parent);
            return current;
        }
        CFRelease(current);
        current = parent;
    }
    CFRelease(current);
    return NULL;
}

static AXUIElementRef copy_selected_column_child(
    AXUIElementRef container
) {
    CFArrayRef selected = copy_array_attribute(
        container,
        kAXSelectedChildrenAttribute
    );
    if (!selected || CFArrayGetCount(selected) == 0) {
        if (selected) CFRelease(selected);
        return NULL;
    }
    AXUIElementRef selected_element =
        (AXUIElementRef)CFArrayGetValueAtIndex(selected, 0);
    AXUIElementRef child = copy_direct_column_child(
        selected_element,
        container
    );
    CFRelease(selected);
    return child;
}
