static CGKeyCode arrow_key_code(direction_t direction) {
    switch (direction) {
        case direction_down: return kVK_DownArrow;
        case direction_up: return kVK_UpArrow;
        case direction_left: return kVK_LeftArrow;
        case direction_right: return kVK_RightArrow;
    }
}

static bool arrow_key_is_down(direction_t direction) {
    return CGEventSourceKeyState(
        kCGEventSourceStateCombinedSessionState,
        arrow_key_code(direction)
    );
}

static CFIndex wait_for_selection_change(
    const navigation_context_t *context,
    CFIndex current
) {
    // Give Finder's main loop time to consume the posted key event before AX
    // polling. Immediate cross-process queries can otherwise keep returning the
    // old selection for the entire polling window.
    usleep(10000);
    for (int attempt = 0; attempt < 20; ++attempt) {
        CFIndex updated = current_index(context);
        if (updated >= 0 && updated != current) return updated;
        usleep(500);
    }
    return current;
}

static CFIndex selectable_vertical_edge_index(
    const navigation_context_t *context,
    bool last
) {
    CFIndex count = CFArrayGetCount(context->items);
    if (count <= 0) return -1;

    CFIndex index = last ? count - 1 : 0;
    CFIndex step = last ? -1 : 1;
    for (; index >= 0 && index < count; index += step) {
        if (navigation_item_is_selectable(context, index)) return index;
    }

    return -1;
}

static CFIndex select_vertical_edge(
    const navigation_context_t *context,
    bool last
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex index = selectable_vertical_edge_index(context, last);
    CFIndex step = last ? -1 : 1;
    for (; index >= 0 && index < count; index += step) {
        if (!select_index(context, index)) continue;
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (current_index(context) == index) return index + 1;
            usleep(500);
        }
    }
    return 0;
}

static CFIndex wrap_vertical_edge(
    const navigation_context_t *context,
    direction_t direction
) {
    bool last = direction == direction_up;
    if (context->role == navigation_outline
        && !context->has_marks
        && post_vertical_wrap_chord(context->finder_pid, direction)) {
        CFIndex edge = selectable_vertical_edge_index(context, last);
        return edge >= 0 ? edge + 1 : 0;
    }
    CFIndex position = select_vertical_edge(context, last);
    if (position <= 0) return 0;
    // Finder can apply the AX selection before its Column scroll hierarchy is
    // writable. Request the edge scroll, but keep the successful selection as
    // the source of truth; the still-held native arrow then owns visibility.
    scroll_vertical_selection_to_edge(context, position - 1, last);
    return position;
}

static CFIndex select_outline_next(
    const navigation_context_t *context,
    CFIndex current,
    direction_t direction
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex index = current;
    for (CFIndex attempt = 0; attempt < count; ++attempt) {
        if (direction == direction_down) {
            index = index < 0 ? 0 : (index + 1) % count;
        } else {
            index = index < 0 ? count - 1 : (index + count - 1) % count;
        }
        bool wrapped = current >= 0
            && ((direction == direction_down && index < current)
                || (direction == direction_up && index > current));
        if (wrapped && !context->has_marks
            && post_vertical_wrap_chord(context->finder_pid, direction)) {
            CFIndex edge = selectable_vertical_edge_index(
                context,
                direction == direction_up
            );
            return edge >= 0 ? edge + 1 : 0;
        }
        if (!select_index(context, index)) continue;
        for (int wait_attempt = 0; wait_attempt < 40; ++wait_attempt) {
            if (current_index(context) == index) {
                if (wrapped && !scroll_vertical_selection_to_edge(
                        context,
                        index,
                        direction == direction_up
                    )) {
                    return 0;
                }
                return index + 1;
            }
            usleep(500);
        }
    }
    return 0;
}

static CFIndex move_vertical_count(
    navigation_context_t *context,
    direction_t direction,
    CFIndex requested_count
) {
    if ((direction != direction_down && direction != direction_up)
        || requested_count <= 0) {
        return 0;
    }

    refresh_visible_marks(context);
    CFIndex item_count = CFArrayGetCount(context->items);
    CFIndex anchor = context->cursor_index >= 0
        ? context->cursor_index
        : take_navigation_anchor(context);
    CFIndex current = anchor >= 0 ? anchor : current_index(context);
    CFIndex destination = current;

    if (context->role == navigation_outline
        || context->role == navigation_list) {
        CFIndex candidate = current;
        CFIndex moved = 0;
        CFIndex step = direction == direction_down ? 1 : -1;
        while (moved < requested_count) {
            candidate += step;
            if (candidate < 0 || candidate >= item_count) break;
            if (!navigation_item_is_selectable(context, candidate)) continue;
            destination = candidate;
            ++moved;
        }
        if (destination < 0) {
            destination = selectable_vertical_edge_index(
                context,
                direction == direction_up
            );
        }
    } else if (direction == direction_down) {
        CFIndex start = current < 0 ? -1 : current;
        destination = start + requested_count;
        if (destination >= item_count) destination = item_count - 1;
    } else {
        CFIndex start = current < 0 ? item_count : current;
        destination = start - requested_count;
        if (destination < 0) destination = 0;
    }

    if (destination < 0 || destination >= item_count) {
        return 0;
    }

    bool selected = false;
    bool native_visibility = false;
    if (context->role == navigation_outline && !context->has_marks) {
        CFIndex first = selectable_vertical_edge_index(context, false);
        CFIndex last = selectable_vertical_edge_index(context, true);
        if (destination == first || destination == last) {
            selected = post_vertical_wrap_chord(
                context->finder_pid,
                destination == first ? direction_down : direction_up
            );
            native_visibility = selected;
        } else {
            CFIndex stage = destination
                + (direction == direction_down ? -1 : 1);
            CFIndex stage_step = direction == direction_down ? -1 : 1;
            while (stage >= 0 && stage < item_count) {
                AXUIElementRef stage_item =
                    (AXUIElementRef)CFArrayGetValueAtIndex(
                        context->items,
                        stage
                    );
                CFStringRef stage_url = copy_navigation_item_url_string(
                    stage_item,
                    0
                );
                if (stage_url) {
                    CFRelease(stage_url);
                    break;
                }
                stage += stage_step;
            }
            if (stage >= 0 && stage < item_count
                && select_index(context, stage)) {
                set_attribute(
                    context->container,
                    kAXFocusedAttribute,
                    kCFBooleanTrue
                );
                selected = post_key_code(arrow_key_code(direction));
                native_visibility = selected;
            }
        }
    }
    if (!selected) {
        selected = set_visible_mark_and_cursor_selection(
            context,
            destination
        );
    }
    if (!selected) {
        return 0;
    }

    if (context->has_marks) {
        context->cursor_index = destination;
    } else {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (current_index(context) == destination) break;
            usleep(500);
        }
    }

    if (!native_visibility) {
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            destination
        );
        perform_ax_action(item, CFSTR("AXScrollToVisible"));
    }
    if (!write_navigation_anchor(context)) return 0;
    return destination + 1;
}

static CFIndex grid_column_index(
    const navigation_context_t *context,
    CFIndex index
) {
    AXUIElementRef target = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        index
    );
    CGPoint target_position;
    if (!position_of(target, &target_position)) return 0;

    CFIndex column = 0;
    CFIndex count = CFArrayGetCount(context->items);
    for (CFIndex item_index = 0; item_index < count; ++item_index) {
        CGPoint position;
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            item_index
        );
        if (position_of(item, &position)
            && fabs(position.y - target_position.y) <= 8.0
            && position.x < target_position.x) {
            ++column;
        }
    }
    return column;
}

static CFIndex grid_row_index(
    const navigation_context_t *context,
    CFIndex index
) {
    if (index <= 0) return 0;

    CGPoint previous_position;
    AXUIElementRef first_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        0
    );
    if (!position_of(first_item, &previous_position)) return 0;

    CFIndex row = 0;
    for (CFIndex item_index = 1; item_index <= index; ++item_index) {
        CGPoint position;
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            item_index
        );
        if (!position_of(item, &position)) continue;
        if (fabs(position.y - previous_position.y) > 8.0) ++row;
        previous_position = position;
    }
    return row;
}

static bool same_grid_row(
    const navigation_context_t *context,
    CFIndex first,
    CFIndex second
) {
    CGPoint first_position;
    CGPoint second_position;
    AXUIElementRef first_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        first
    );
    AXUIElementRef second_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        second
    );
    return position_of(first_item, &first_position)
        && position_of(second_item, &second_position)
        && fabs(first_position.y - second_position.y) <= 8.0;
}

static CFIndex maximum_grid_column(
    const navigation_context_t *context
) {
    CFIndex maximum = 0;
    CFIndex count = CFArrayGetCount(context->items);
    for (CFIndex index = 0; index < count; ++index) {
        CFIndex column = grid_column_index(context, index);
        if (column > maximum) maximum = column;
    }
    return maximum;
}

static CFIndex select_grid_document_edge(
    navigation_context_t *context,
    bool last
) {
    CFIndex old_count = CFArrayGetCount(context->items);
    AXUIElementRef old_first = old_count > 0
        ? (AXUIElementRef)CFRetain(CFArrayGetValueAtIndex(context->items, 0))
        : NULL;
    AXUIElementRef old_last = old_count > 0
        ? (AXUIElementRef)CFRetain(
            CFArrayGetValueAtIndex(context->items, old_count - 1)
        )
        : NULL;
    if (!set_vertical_scroll_edge(context, last, NULL)) {
        if (old_last) CFRelease(old_last);
        if (old_first) CFRelease(old_first);
        return 0;
    }

    CFArrayRef replacement = NULL;
    for (unsigned attempt = 0; attempt < 80; ++attempt) {
        if (attempt == 0) usleep(5000);
        replacement = copy_navigation_items(
            context->container,
            context->role
        );
        if (replacement && CFArrayGetCount(replacement) > 0) {
            CFIndex replacement_count = CFArrayGetCount(replacement);
            AXUIElementRef replacement_first =
                (AXUIElementRef)CFArrayGetValueAtIndex(replacement, 0);
            AXUIElementRef replacement_last =
                (AXUIElementRef)CFArrayGetValueAtIndex(
                    replacement,
                    replacement_count - 1
                );
            bool changed = replacement_count != old_count
                || !old_first
                || !old_last
                || !CFEqual(old_first, replacement_first)
                || !CFEqual(old_last, replacement_last);
            if (changed || attempt == 79) break;
        }
        if (replacement) {
            CFRelease(replacement);
            replacement = NULL;
        }
        usleep(1000);
    }
    if (old_last) CFRelease(old_last);
    if (old_first) CFRelease(old_first);
    if (!replacement || CFArrayGetCount(replacement) == 0) {
        if (replacement) CFRelease(replacement);
        return 0;
    }

    CFRelease(context->items);
    context->items = replacement;
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex target = last ? count - 1 : 0;
    if (!select_index(context, target)) return 0;
    AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        target
    );
    perform_ax_action(item, CFSTR("AXScrollToVisible"));
    context->predicted_index = target;
    return target + 1;
}

static CFIndex move_grid_horizontal(
    navigation_context_t *context,
    CFIndex current,
    direction_t direction,
    bool *navigation_may_change
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex target = direction == direction_right
        ? (current + 1) % count
        : (current + count - 1) % count;
    if (same_grid_row(context, current, target)) {
        if (!post_key_code(arrow_key_code(direction))) return 0;
        return target + 1;
    }

    double scroll_value = 0.0;
    bool has_scroll_value = vertical_scroll_value(context, &scroll_value);
    if (direction == direction_right) {
        if (target == 0) {
            if (has_scroll_value && scroll_value < 0.999) {
                if (!post_key_code(kVK_DownArrow)) return 0;
                CFIndex column = grid_column_index(context, current);
                for (CFIndex step = 0; step < column; ++step) {
                    post_key_code(kVK_LeftArrow);
                }
                wait_for_selection_change(context, current);
                if (navigation_may_change) *navigation_may_change = true;
                return current + 1;
            }
            if (has_scroll_value) {
                return select_grid_document_edge(context, false);
            }
            CFIndex row = grid_row_index(context, current);
            CFIndex column = grid_column_index(context, current);
            for (CFIndex step = 0; step < row; ++step) {
                post_key_code(kVK_UpArrow);
            }
            for (CFIndex step = 0; step < column; ++step) {
                post_key_code(kVK_LeftArrow);
            }
        } else {
            if (!post_key_code(kVK_DownArrow)) return 0;
            CFIndex column = grid_column_index(context, current);
            for (CFIndex step = 0; step < column; ++step) {
                post_key_code(kVK_LeftArrow);
            }
        }
    } else {
        if (current == 0) {
            if (has_scroll_value && scroll_value > 0.001) {
                if (!post_key_code(kVK_UpArrow)) return 0;
                CFIndex target_column = maximum_grid_column(context);
                for (CFIndex step = 0; step < target_column; ++step) {
                    post_key_code(kVK_RightArrow);
                }
                wait_for_selection_change(context, current);
                if (navigation_may_change) *navigation_may_change = true;
                return current + 1;
            }
            if (has_scroll_value) {
                return select_grid_document_edge(context, true);
            }
            CFIndex target_row = grid_row_index(context, target);
            CFIndex target_column = grid_column_index(context, target);
            for (CFIndex step = 0; step < target_row; ++step) {
                post_key_code(kVK_DownArrow);
            }
            for (CFIndex step = 0; step < target_column; ++step) {
                post_key_code(kVK_RightArrow);
            }
        } else {
            if (!post_key_code(kVK_UpArrow)) return 0;
            CFIndex target_column = grid_column_index(context, target);
            for (CFIndex step = 0; step < target_column; ++step) {
                post_key_code(kVK_RightArrow);
            }
        }
    }

    return target + 1;
}

static bool post_column_navigation_key(
    CGKeyCode key_code,
    column_phase_metrics_t *column_phases
) {
    if (!column_phases) return post_key_code(key_code);

    uint64_t started_nanoseconds = monotonic_nanoseconds();
    bool success = post_key_code(key_code);
    column_phases->event_post_nanoseconds += counter_delta(
        monotonic_nanoseconds(),
        started_nanoseconds
    );
    return success;
}

static const CFIndex current_index_unknown = (CFIndex)-2;

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
    CFIndex anchor = context->cursor_index >= 0
        ? context->cursor_index
        : use_navigation_anchor
            ? take_navigation_anchor(context)
            : -1;
    if (anchor >= 0 && !context->has_marks
        && !select_index(context, anchor)) {
        anchor = -1;
    }
    if (anchor >= 0) context->predicted_index = -1;
    CFIndex current = anchor >= 0
        ? anchor
        : context->role == navigation_grid
            && context->predicted_index >= 0
        ? context->predicted_index
        : current_hint != current_index_unknown
            ? current_hint
            : current_index(context);
    if (context->role == navigation_grid) {
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
        if (context->has_marks && position > 0
            && direction != direction_left
            && direction != direction_right) {
            if (!set_visible_mark_and_cursor_selection(
                    context,
                    position - 1
                )) {
                position = 0;
            }
        }
        if (context->has_marks && position > 0) {
            context->cursor_index = position - 1;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return position;
    }

    if (context->role == navigation_outline
        && (direction == direction_down || direction == direction_up)) {
        CFIndex position = select_outline_next(context, current, direction);
        if (context->has_marks && position > 0
            && !set_visible_mark_and_cursor_selection(
                context,
                position - 1
            )) {
            position = 0;
        }
        if (context->has_marks && position > 0) {
            context->cursor_index = position - 1;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return position;
    }

    if (uses_descendant_selection(context, current)) {
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
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return position;
    }

    if (direction == direction_left || direction == direction_right) {
        bool success = post_column_navigation_key(
            arrow_key_code(direction),
            column_phases
        );
        if (success && navigation_may_change) {
            *navigation_may_change = true;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return success ? (current >= 0 ? current + 1 : 1) : 0;
    }

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
    if (!navigation_item_is_selectable(context, destination)) {
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return 0;
    }

    bool success = set_visible_mark_and_cursor_selection(
        context,
        destination
    );
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
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    return success ? destination + 1 : 0;
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
        for (CFIndex step = 0; step < steps; ++step) {
            if (!post_key_code(key_code)) return 0;
        }
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
