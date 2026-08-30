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
                (void)post_key_code_repeat(kVK_LeftArrow, column);
                wait_for_selection_change(context, current);
                if (navigation_may_change) *navigation_may_change = true;
                return current + 1;
            }
            if (has_scroll_value) {
                return select_grid_document_edge(context, false);
            }
            CFIndex row = grid_row_index(context, current);
            CFIndex column = grid_column_index(context, current);
            (void)post_key_code_repeat(kVK_UpArrow, row);
            (void)post_key_code_repeat(kVK_LeftArrow, column);
        } else {
            if (!post_key_code(kVK_DownArrow)) return 0;
            CFIndex column = grid_column_index(context, current);
            (void)post_key_code_repeat(kVK_LeftArrow, column);
        }
    } else {
        if (current == 0) {
            if (has_scroll_value && scroll_value > 0.001) {
                if (!post_key_code(kVK_UpArrow)) return 0;
                CFIndex target_column = maximum_grid_column(context);
                (void)post_key_code_repeat(kVK_RightArrow, target_column);
                wait_for_selection_change(context, current);
                if (navigation_may_change) *navigation_may_change = true;
                return current + 1;
            }
            if (has_scroll_value) {
                return select_grid_document_edge(context, true);
            }
            CFIndex target_row = grid_row_index(context, target);
            CFIndex target_column = grid_column_index(context, target);
            (void)post_key_code_repeat(kVK_DownArrow, target_row);
            (void)post_key_code_repeat(kVK_RightArrow, target_column);
        } else {
            if (!post_key_code(kVK_UpArrow)) return 0;
            CFIndex target_column = grid_column_index(context, target);
            (void)post_key_code_repeat(kVK_RightArrow, target_column);
        }
    }

    return target + 1;
}
