static CFIndex current_index(const navigation_context_t *context) {
    CFIndex item_count = CFArrayGetCount(context->items);
    CFStringRef selected_attribute = context->role == navigation_outline
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute;
    CFArrayRef selected = copy_array_attribute(
        context->container,
        selected_attribute
    );
    if (selected && CFArrayGetCount(selected) > 0) {
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
        if (index != kCFNotFound) return index;
    } else if (selected) {
        CFRelease(selected);
    }

    // Compatibility fallback when the container does not expose its selected
    // rows/children. This is intentionally kept off the common path.
    for (CFIndex index = 0; index < item_count; ++index) {
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            index
        );
        if (context->role == navigation_outline) {
            if (bool_attribute(item, kAXSelectedAttribute)) return index;
        } else if (descendant_selected(item, 0)) {
            return index;
        }
    }
    return -1;
}

static bool selected_item_is_visible(
    const navigation_context_t *context
) {
    if (context->role != navigation_outline) return false;
    CFIndex selected_index = current_index(context);
    if (selected_index < 0) return false;

    AXUIElementRef selected_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        selected_index
    );
    CFStringRef selected_url = copy_navigation_item_url_string(
        selected_item,
        0
    );
    if (!selected_url) return false;

    CFArrayRef visible_rows = copy_array_attribute(
        context->container,
        kAXVisibleRowsAttribute
    );
    bool visible = false;
    if (visible_rows) {
        CFIndex visible_count = CFArrayGetCount(visible_rows);
        for (CFIndex index = 0; index < visible_count; ++index) {
            AXUIElementRef row = (AXUIElementRef)CFArrayGetValueAtIndex(
                visible_rows,
                index
            );
            if (navigation_item_matches_url(row, selected_url)) {
                visible = true;
                break;
            }
        }
        CFRelease(visible_rows);
    }
    CFRelease(selected_url);
    return visible;
}

static bool select_index(const navigation_context_t *context, CFIndex index) {
    if (!navigation_item_is_selectable(context, index)) return false;
    AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(context->items, index);
    const void *values[] = {item};
    CFArrayRef selection = CFArrayCreate(
        kCFAllocatorDefault,
        values,
        1,
        &kCFTypeArrayCallBacks
    );

    AXError error;
    if (context->role == navigation_list || context->role == navigation_grid) {
        error = set_attribute(
            context->container,
            kAXSelectedChildrenAttribute,
            selection
        );
        set_attribute(
            context->container,
            kAXFocusedAttribute,
            kCFBooleanTrue
        );
    } else {
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
    }
    CFRelease(selection);
    return error == kAXErrorSuccess;
}
