static bool post_column_navigation_key(
    CGKeyCode key_code,
    column_phase_metrics_t *column_phases
) {
    uint64_t started_nanoseconds = phase_timer_start(column_phases);
    bool success = post_key_code(key_code);
    PHASE_TIMER_ADD(column_phases, event_post_nanoseconds, started_nanoseconds);
    return success;
}

static const CFIndex current_index_unknown = (CFIndex)-2;

// The movement handlers below never touch the movement lock. `move_once` owns
// it for the whole dispatch so every outcome leaves through one unlock.

static CFIndex move_once_grid(
    navigation_context_t *context,
    direction_t direction,
    CFIndex current,
    CFIndex count,
    bool *navigation_may_change
) {
    CFIndex position = 0;
    if (current >= 0
        && (direction == direction_left || direction == direction_right)) {
        if (context->has_marks) {
            CFIndex destination = direction == direction_right
                ? (current + 1) % count
                : (current + count - 1) % count;
            position = set_visible_mark_and_cursor_selection(
                context,
                destination
            ) ? destination + 1 : 0;
        } else {
            position = move_grid_horizontal(
                context,
                current,
                direction,
                navigation_may_change
            );
        }
    } else if (current >= 0 && post_key_code(arrow_key_code(direction))) {
        CFIndex destination = context->has_marks
            ? wait_for_selection_change(context, current)
            : current;
        position = destination >= 0 ? destination + 1 : 0;
    }
    // Vertical Grid movement posts a native arrow and only then learns where
    // Finder landed, so the marked selection has to be rebuilt afterwards.
    // The horizontal paths above already selected through the mark helper.
    if (context->has_marks && position > 0
        && direction != direction_left
        && direction != direction_right) {
        if (!set_visible_mark_and_cursor_selection(context, position - 1)) {
            position = 0;
        }
    }
    if (context->has_marks && position > 0) {
        context->cursor_index = position - 1;
    }
    return position;
}

static CFIndex move_once_outline_vertical(
    navigation_context_t *context,
    direction_t direction,
    CFIndex current
) {
    CFIndex position = select_outline_next(context, current, direction);
    if (context->has_marks && position > 0
        && !set_visible_mark_and_cursor_selection(context, position - 1)) {
        position = 0;
    }
    if (context->has_marks && position > 0) {
        context->cursor_index = position - 1;
    }
    return position;
}

static CFIndex move_once_descendant_selection(
    navigation_context_t *context,
    direction_t direction,
    CFIndex current,
    bool *navigation_may_change,
    column_phase_metrics_t *column_phases
) {
    CFIndex position;
    if (direction == direction_left || direction == direction_right) {
        bool success = post_column_navigation_key(
            arrow_key_code(direction),
            column_phases
        );
        if (success && navigation_may_change) {
            *navigation_may_change = true;
        }
        position = success ? (current >= 0 ? current + 1 : 1) : 0;
    } else {
        bool success = post_column_navigation_key(
            arrow_key_code(direction),
            column_phases
        );
        CFIndex destination = success
            ? wait_for_selection_change(context, current)
            : -1;
        if (context->has_marks && destination >= 0
            && !set_visible_mark_and_cursor_selection(
                context,
                destination
            )) {
            destination = -1;
        }
        position = destination >= 0 ? destination + 1 : 0;
    }
    if (context->has_marks && position > 0) {
        context->cursor_index = position - 1;
    }
    return position;
}

// Column hierarchy movement. The new directory is discovered by the caller
// through `navigation_may_change`, so no cursor bookkeeping applies here.
static CFIndex move_once_horizontal(
    direction_t direction,
    CFIndex current,
    bool *navigation_may_change,
    column_phase_metrics_t *column_phases
) {
    bool success = post_column_navigation_key(
        arrow_key_code(direction),
        column_phases
    );
    if (success && navigation_may_change) {
        *navigation_may_change = true;
    }
    return success ? (current >= 0 ? current + 1 : 1) : 0;
}

static CFIndex move_once_indexed(
    navigation_context_t *context,
    direction_t direction,
    CFIndex current,
    CFIndex count
) {
    CFIndex destination = current;
    for (CFIndex attempt = 0; attempt < count; ++attempt) {
        if (direction == direction_down) {
            destination = destination < 0
                ? 0
                : (destination + 1) % count;
        } else {
            destination = destination < 0
                ? count - 1
                : (destination + count - 1) % count;
        }
        if (navigation_item_is_selectable(context, destination)) break;
    }
    if (!navigation_item_is_selectable(context, destination)) return 0;

    bool success = set_visible_mark_and_cursor_selection(context, destination);
    if (success) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (context->has_marks || current_index(context) == destination) {
                break;
            }
            usleep(500);
        }
    }
    if (success && context->has_marks) {
        context->cursor_index = destination;
    }
    return success ? destination + 1 : 0;
}

static CFIndex move_once_current_index(
    navigation_context_t *context,
    CFIndex current_hint,
    bool use_navigation_anchor
) {
    CFIndex anchor = context->cursor_index >= 0
        ? context->cursor_index
        : use_navigation_anchor
            ? take_navigation_anchor(context)
            : -1;
    if (anchor >= 0 && !context->has_marks
        && !select_index(context, anchor)) {
        anchor = -1;
    }
    if (anchor >= 0) {
        context->predicted_index = -1;
        return anchor;
    }
    if (context->role == navigation_grid && context->predicted_index >= 0) {
        return context->predicted_index;
    }
    return current_hint != current_index_unknown
        ? current_hint
        : current_index(context);
}

static CFIndex move_once(
    navigation_context_t *context,
    direction_t direction,
    CFIndex current_hint,
    int lock_fd,
    bool *navigation_may_change,
    bool use_navigation_anchor,
    column_phase_metrics_t *column_phases
) {
    if (navigation_may_change) *navigation_may_change = false;
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    if (use_navigation_anchor || !context->marks_loaded) {
        refresh_visible_marks(context);
    }

    CFIndex count = CFArrayGetCount(context->items);
    CFIndex current = move_once_current_index(
        context,
        current_hint,
        use_navigation_anchor
    );

    CFIndex position;
    if (context->role == navigation_grid) {
        position = move_once_grid(
            context,
            direction,
            current,
            count,
            navigation_may_change
        );
    } else if (context->role == navigation_outline
            && (direction == direction_down || direction == direction_up)) {
        position = move_once_outline_vertical(context, direction, current);
    } else if (uses_descendant_selection(context, current)) {
        position = move_once_descendant_selection(
            context,
            direction,
            current,
            navigation_may_change,
            column_phases
        );
    } else if (direction == direction_left || direction == direction_right) {
        position = move_once_horizontal(
            direction,
            current,
            navigation_may_change,
            column_phases
        );
    } else {
        position = move_once_indexed(context, direction, current, count);
    }

    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    return position;
}

static CFIndex move_to_edge(
    navigation_context_t *context,
    bool last
) {
    refresh_visible_marks(context);
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex anchor = take_navigation_anchor(context);
    CFIndex current = anchor >= 0 ? anchor : current_index(context);
    if (context->has_marks && current >= 0 && !select_index(context, current)) {
        return 0;
    }
    if (context->role == navigation_grid
        || uses_descendant_selection(context, current)) {
        if (current < 0) return 0;
        AXUIElementRef current_item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            current
        );
        CGPoint current_position;
        if (!position_of(current_item, &current_position)) return 0;

        CFIndex target = current;
        CGFloat target_y = current_position.y;
        for (CFIndex index = 0; index < count; ++index) {
            AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
                context->items,
                index
            );
            CGPoint position;
            if (!position_of(item, &position)
                || fabs(position.x - current_position.x) > 8.0) {
                continue;
            }
            if ((last && position.y > target_y)
                || (!last && position.y < target_y)) {
                target = index;
                target_y = position.y;
            }
        }

        CFIndex current_row = grid_row_index(context, current);
        CFIndex target_row = grid_row_index(context, target);
        CFIndex steps = current_row > target_row
            ? current_row - target_row
            : target_row - current_row;
        CGKeyCode key_code = last ? kVK_DownArrow : kVK_UpArrow;
        if (!post_key_code_repeat(key_code, steps)) return 0;
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (current_index(context) == target) break;
            usleep(500);
        }
        CFIndex actual = current_index(context);
        if (actual < 0) return 0;
        if (context->has_marks
            && !set_visible_mark_and_cursor_selection(context, actual)) {
            return 0;
        }
        if (context->has_marks) context->cursor_index = actual;
        if (!write_navigation_anchor(context)) return 0;
        return actual + 1;
    }

    if (context->role == navigation_outline) {
        if (!context->has_marks
            && post_vertical_wrap_chord(
                context->finder_pid,
                last ? direction_up : direction_down
            )) {
            CFIndex edge = selectable_vertical_edge_index(context, last);
            return edge >= 0 ? edge + 1 : 0;
        }
        CFIndex position = select_vertical_edge(context, last);
        if (position <= 0) return 0;
        if (context->has_marks
            && !set_visible_mark_and_cursor_selection(
                context,
                position - 1
            )) {
            return 0;
        }
        if (context->has_marks) context->cursor_index = position - 1;
        if (!scroll_vertical_selection_to_edge(
                context,
                position - 1,
                last
            )) {
            return 0;
        }
        if (!write_navigation_anchor(context)) return 0;
        return position;
    }

    CFIndex target = selectable_vertical_edge_index(context, last);
    if (target < 0) return 0;
    bool success = set_visible_mark_and_cursor_selection(context, target);
    if (success) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (context->has_marks || current_index(context) == target) break;
            usleep(500);
        }
    }
    if (success) {
        if (context->has_marks) context->cursor_index = target;
        success = write_navigation_anchor(context);
    }
    return success ? target + 1 : 0;
}
