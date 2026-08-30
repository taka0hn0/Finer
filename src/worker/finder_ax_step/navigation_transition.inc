static bool focused_navigation_container_changed(
    AXUIElementRef previous_container,
    column_phase_metrics_t *column_phases
) {
    uint64_t focus_started_nanoseconds = phase_timer_start(column_phases);
    AXUIElementRef application = NULL;
    if (!finder_is_frontmost(NULL, &application)) {
        PHASE_TIMER_ADD(
            column_phases,
            transition_focus_nanoseconds,
            focus_started_nanoseconds
        );
        return false;
    }

    CFTypeRef focused = copy_attribute(application, kAXFocusedUIElementAttribute);
    CFRelease(application);
    if (!focused || CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        PHASE_TIMER_ADD(
            column_phases,
            transition_focus_nanoseconds,
            focus_started_nanoseconds
        );
        return false;
    }

    navigation_role_t role;
    AXUIElementRef current = copy_nearest_navigation_container(
        (AXUIElementRef)focused,
        &role
    );
    CFRelease(focused);
    PHASE_TIMER_ADD(
        column_phases,
        transition_focus_nanoseconds,
        focus_started_nanoseconds
    );
    if (!current) return false;
    bool changed = !CFEqual(current, previous_container);
    if (changed) {
        uint64_t items_started_nanoseconds = phase_timer_start(column_phases);
        CFArrayRef items = copy_navigation_items(current, role);
        PHASE_TIMER_ADD(
            column_phases,
            transition_candidate_items_nanoseconds,
            items_started_nanoseconds
        );
        navigation_context_t candidate = {
            .container = current,
            .items = items,
            .role = role,
        };
        if (items && CFArrayGetCount(items) > 0) {
            uint64_t selection_started_nanoseconds =
                phase_timer_start(column_phases);
            changed = current_index(&candidate) >= 0;
            PHASE_TIMER_ADD(
                column_phases,
                transition_candidate_selection_nanoseconds,
                selection_started_nanoseconds
            );
        } else {
            changed = false;
        }
        if (items) CFRelease(items);
    }
    CFRelease(current);
    return changed;
}

static void wait_for_navigation_transition(
    const navigation_context_t *context,
    CFIndex previous_item_count,
    column_phase_metrics_t *column_phases
) {
    // CGEventPost is asynchronous. Serialize the following Vim command behind
    // Finder's view update so it cannot mutate the previous directory.
    uint64_t transition_started_nanoseconds = phase_timer_start(column_phases);
    bool transitioned = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (column_phases) ++column_phases->transition_attempts;
        uint64_t item_count_started_nanoseconds =
            phase_timer_start(column_phases);
        CFIndex item_count = raw_navigation_item_count(
            context->container,
            context->role
        );
        PHASE_TIMER_ADD(
            column_phases,
            transition_item_count_nanoseconds,
            item_count_started_nanoseconds
        );
        if (previous_item_count >= 0 && item_count != previous_item_count) {
            if (column_phases) {
                column_phases->transition_reason = column_transition_item_count;
            }
            transitioned = true;
            break;
        }
        if (focused_navigation_container_changed(
                context->container,
                column_phases
            )) {
            if (column_phases) {
                column_phases->transition_reason =
                    column_transition_focused_container;
            }
            transitioned = true;
            break;
        }
        uint64_t sleep_started_nanoseconds = phase_timer_start(column_phases);
        usleep(2000);
        PHASE_TIMER_ADD(
            column_phases,
            transition_sleep_nanoseconds,
            sleep_started_nanoseconds
        );
    }
    if (column_phases && !transitioned) {
        column_phases->transition_reason = column_transition_timeout;
    }
    PHASE_TIMER_ADD(
        column_phases,
        transition_total_nanoseconds,
        transition_started_nanoseconds
    );
}

static CFIndex move_worker_context(
    navigation_context_t *context,
    bool *context_created,
    direction_t direction,
    CFIndex current,
    int movement_lock_fd,
    bool use_navigation_anchor,
    column_phase_metrics_t *column_phases
) {
    CFIndex previous_item_count = -1;
    if (direction == direction_left || direction == direction_right) {
        uint64_t item_count_started_nanoseconds =
            phase_timer_start(column_phases);
        previous_item_count = raw_navigation_item_count(
            context->container,
            context->role
        );
        PHASE_TIMER_ADD(
            column_phases,
            previous_item_count_nanoseconds,
            item_count_started_nanoseconds
        );
    }
    bool navigation_may_change = false;
    uint64_t movement_started_nanoseconds = phase_timer_start(column_phases);
    CFIndex position = move_once(
        context,
        direction,
        current,
        movement_lock_fd,
        &navigation_may_change,
        use_navigation_anchor,
        column_phases
    );
    PHASE_TIMER_ADD(
        column_phases,
        movement_nanoseconds,
        movement_started_nanoseconds
    );
    if (context->role == navigation_grid) {
        context->predicted_index = position > 0
                && (direction == direction_left || direction == direction_right)
            ? position - 1
            : -1;
    }
    if (navigation_may_change) {
        if (context->role != navigation_grid) {
            wait_for_navigation_transition(
                context,
                previous_item_count,
                column_phases
            );
        }
        navigation_context_release(context);
        *context_created = false;
    }
    return position;
}
