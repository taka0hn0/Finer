static bool clear_selection(navigation_context_t *context) {
    CFStringRef selected_attribute = context->role == navigation_outline
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute;
    CFArrayRef empty_selection = CFArrayCreate(
        kCFAllocatorDefault,
        NULL,
        0,
        &kCFTypeArrayCallBacks
    );
    AXError error = set_attribute(
        context->container,
        selected_attribute,
        empty_selection
    );
    CFRelease(empty_selection);

    if (error != kAXErrorSuccess && context->role == navigation_outline) {
        bool fallback_succeeded = true;
        CFIndex count = CFArrayGetCount(context->items);
        for (CFIndex index = 0; index < count; ++index) {
            AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
                context->items,
                index
            );
            if (!bool_attribute(item, kAXSelectedAttribute)) continue;
            if (set_attribute(
                    item,
                    kAXSelectedAttribute,
                    kCFBooleanFalse
                ) != kAXErrorSuccess) {
                fallback_succeeded = false;
            }
        }
        if (fallback_succeeded) error = kAXErrorSuccess;
    }

    if (error == kAXErrorSuccess) {
        context->predicted_index = -1;
        set_attribute(
            context->container,
            kAXFocusedAttribute,
            kCFBooleanTrue
        );
    }
    return error == kAXErrorSuccess;
}

static bool clear_selection_array_attribute(
    AXUIElementRef element,
    CFStringRef attribute
) {
    CFArrayRef selected = copy_array_attribute(element, attribute);
    if (!selected) return false;

    CFArrayRef empty_selection = CFArrayCreate(
        kCFAllocatorDefault,
        NULL,
        0,
        &kCFTypeArrayCallBacks
    );
    AXError error = set_attribute(element, attribute, empty_selection);
    CFRelease(empty_selection);
    if (error == kAXErrorSuccess) {
        CFRelease(selected);
        return true;
    }

    bool cleared = CFArrayGetCount(selected) > 0;
    CFIndex count = CFArrayGetCount(selected);
    for (CFIndex index = 0; index < count; ++index) {
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            selected,
            index
        );
        if (set_attribute(
                item,
                kAXSelectedAttribute,
                kCFBooleanFalse
            ) != kAXErrorSuccess) {
            cleared = false;
        }
    }
    CFRelease(selected);
    return cleared;
}

static void clear_selected_descendants(
    AXUIElementRef element,
    int depth,
    bool *found,
    bool *failed
) {
    if (bool_attribute(element, kAXSelectedAttribute)) {
        *found = true;
        if (set_attribute(
                element,
                kAXSelectedAttribute,
                kCFBooleanFalse
            ) != kAXErrorSuccess) {
            *failed = true;
        }
    }
    if (depth >= 4) return;

    CFArrayRef children = copy_array_attribute(element, kAXChildrenAttribute);
    if (!children) return;
    CFIndex count = CFArrayGetCount(children);
    for (CFIndex index = 0; index < count; ++index) {
        clear_selected_descendants(
            (AXUIElementRef)CFArrayGetValueAtIndex(children, index),
            depth + 1,
            found,
            failed
        );
    }
    CFRelease(children);
}

static bool clear_focused_selection_fallback(void) {
    AXUIElementRef application = NULL;
    if (!finder_is_frontmost(NULL, &application)) return false;

    AXUIElementRef system_wide = AXUIElementCreateSystemWide();
    CFTypeRef focused_value = copy_attribute(
        system_wide,
        kAXFocusedUIElementAttribute
    );
    CFRelease(system_wide);
    if (!focused_value
        || CFGetTypeID(focused_value) != AXUIElementGetTypeID()) {
        if (focused_value) CFRelease(focused_value);
        focused_value = copy_attribute(
            application,
            kAXFocusedUIElementAttribute
        );
    }
    CFRelease(application);
    if (!focused_value
        || CFGetTypeID(focused_value) != AXUIElementGetTypeID()) {
        if (focused_value) CFRelease(focused_value);
        return false;
    }

    AXUIElementRef focused = (AXUIElementRef)focused_value;
    AXUIElementRef current = (AXUIElementRef)CFRetain(focused);
    bool cleared = false;
    for (int depth = 0; depth < 10; ++depth) {
        if (clear_selection_array_attribute(
                current,
                kAXSelectedRowsAttribute
            )) {
            cleared = true;
        }
        if (clear_selection_array_attribute(
                current,
                kAXSelectedChildrenAttribute
            )) {
            cleared = true;
        }
        if (bool_attribute(current, kAXSelectedAttribute)
            && set_attribute(
                current,
                kAXSelectedAttribute,
                kCFBooleanFalse
            ) == kAXErrorSuccess) {
            cleared = true;
        }

        CFTypeRef parent = copy_attribute(current, kAXParentAttribute);
        CFRelease(current);
        if (!parent || CFGetTypeID(parent) != AXUIElementGetTypeID()) {
            if (parent) CFRelease(parent);
            current = NULL;
            break;
        }
        current = (AXUIElementRef)parent;
    }
    if (current) CFRelease(current);

    if (!cleared) {
        bool found = false;
        bool failed = false;
        clear_selected_descendants(focused, 0, &found, &failed);
        cleared = found && !failed;
    }
    CFRelease(focused);
    return cleared;
}
