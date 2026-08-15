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
