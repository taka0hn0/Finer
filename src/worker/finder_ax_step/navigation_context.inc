static void find_best_container(
    AXUIElementRef element,
    int depth,
    AXUIElementRef *best,
    double *best_score
) {
    if (depth > 12) return;

    navigation_role_t role;
    if (role_of(element, &role)) {
        CFArrayRef items = copy_navigation_items(element, role);
        if (items && CFArrayGetCount(items) > 0) {
            double score = x_position(element);
            if (selected_count(element, role, items) > 0) score += 1000000.0;
            if (score > *best_score) {
                if (*best) CFRelease(*best);
                *best = (AXUIElementRef)CFRetain(element);
                *best_score = score;
            }
        }
        if (items) CFRelease(items);
    }

    CFArrayRef children = copy_array_attribute(element, kAXChildrenAttribute);
    if (!children) return;
    CFIndex count = CFArrayGetCount(children);
    for (CFIndex index = 0; index < count; ++index) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, index);
        find_best_container(child, depth + 1, best, best_score);
    }
    CFRelease(children);
}

static AXUIElementRef copy_nearest_navigation_container(
    AXUIElementRef focused,
    navigation_role_t *role
) {
    AXUIElementRef current = (AXUIElementRef)CFRetain(focused);
    for (int depth = 0; depth < 10; ++depth) {
        if (role_of(current, role)) return current;

        CFTypeRef parent = copy_attribute(current, kAXParentAttribute);
        CFRelease(current);
        if (!parent || CFGetTypeID(parent) != AXUIElementGetTypeID()) {
            if (parent) CFRelease(parent);
            return NULL;
        }
        current = (AXUIElementRef)parent;
    }
    CFRelease(current);
    return NULL;
}

static AXUIElementRef copy_navigation_container(
    AXUIElementRef focused,
    AXUIElementRef application,
    navigation_role_t *role
) {
    // The nearest supported ancestor is already the active Finder view.
    // Recursively rescanning its descendants makes a cold key press
    // proportional to the number of displayed files.
    AXUIElementRef nearest = copy_nearest_navigation_container(focused, role);
    if (nearest) return nearest;

    CFTypeRef window = copy_attribute(application, kAXFocusedWindowAttribute);
    if (!window || CFGetTypeID(window) != AXUIElementGetTypeID()) {
        if (window) CFRelease(window);
        return NULL;
    }

    AXUIElementRef best = NULL;
    double best_score = -1;
    find_best_container((AXUIElementRef)window, 0, &best, &best_score);
    CFRelease(window);
    if (best) role_of(best, role);
    return best;
}

static bool navigation_context_create(navigation_context_t *context) {
    memset(context, 0, sizeof(*context));
    context->predicted_index = -1;
    context->cursor_index = -1;

    AXUIElementRef application = NULL;
    if (!finder_is_frontmost(&context->finder_pid, &application)) {
        fprintf(stderr, "finder_ax_step: Finder is not frontmost\n");
        return false;
    }

    CFTypeRef focused = copy_attribute(application, kAXFocusedUIElementAttribute);
    if (!focused || CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        CFRelease(application);
        fprintf(stderr, "finder_ax_step: focused UI element is unavailable\n");
        return false;
    }

    context->container = copy_navigation_container(
        (AXUIElementRef)focused,
        application,
        &context->role
    );
    CFRelease(focused);
    CFRelease(application);
    if (!context->container) {
        fprintf(stderr, "finder_ax_step: navigation container is unavailable\n");
        return false;
    }

    context->items = copy_navigation_items(context->container, context->role);
    if (!context->items || CFArrayGetCount(context->items) == 0) {
        if (context->items) CFRelease(context->items);
        CFRelease(context->container);
        memset(context, 0, sizeof(*context));
        fprintf(stderr, "finder_ax_step: navigation container is empty\n");
        return false;
    }
    if (context->role != navigation_grid) {
        context->item_selectability = calloc(
            (size_t)CFArrayGetCount(context->items),
            sizeof(*context->item_selectability)
        );
        if (!context->item_selectability) {
            CFRelease(context->items);
            CFRelease(context->container);
            memset(context, 0, sizeof(*context));
            fprintf(stderr, "finder_ax_step: item cache is unavailable\n");
            return false;
        }
    }
    return true;
}

static void navigation_context_release(navigation_context_t *context) {
    if (context->visible_marked_items) {
        CFRelease(context->visible_marked_items);
    }
    free(context->item_selectability);
    if (context->items) CFRelease(context->items);
    if (context->container) CFRelease(context->container);
    memset(context, 0, sizeof(*context));
}

static AXUIElementRef copy_navigation_item_ancestor(
    AXUIElementRef element,
    CFArrayRef items
) {
    AXUIElementRef current = (AXUIElementRef)CFRetain(element);
    for (int depth = 0; depth < 8; ++depth) {
        if (CFArrayGetFirstIndexOfValue(
                items,
                CFRangeMake(0, CFArrayGetCount(items)),
                current
            ) != kCFNotFound) {
            return current;
        }
        CFTypeRef parent = copy_attribute(current, kAXParentAttribute);
        if (!parent || CFGetTypeID(parent) != AXUIElementGetTypeID()) {
            if (parent) CFRelease(parent);
            break;
        }
        CFRelease(current);
        current = (AXUIElementRef)parent;
    }
    CFRelease(current);
    return NULL;
}
