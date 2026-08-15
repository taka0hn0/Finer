static bool frontmost_process_pid(pid_t *frontmost_pid) {
    ProcessSerialNumber process_serial_number;
    pid_t pid = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus front_process_error = GetFrontProcess(&process_serial_number);
    OSStatus process_pid_error = front_process_error == noErr
        ? GetProcessPID(&process_serial_number, &pid)
        : front_process_error;
#pragma clang diagnostic pop
    if (front_process_error != noErr || process_pid_error != noErr) return false;

    *frontmost_pid = pid;
    return true;
}

static bool process_is_frontmost(pid_t expected_pid) {
    pid_t frontmost_pid = 0;
    return expected_pid > 0
        && frontmost_process_pid(&frontmost_pid)
        && frontmost_pid == expected_pid;
}

static bool finder_is_frontmost(pid_t *finder_pid, AXUIElementRef *application) {
    pid_t pid = 0;
    if (!frontmost_process_pid(&pid)) return false;

    char process_name[PROC_PIDPATHINFO_MAXSIZE] = {0};
    proc_name(pid, process_name, sizeof(process_name));
    if (strcmp(process_name, "Finder") != 0) return false;

    if (finder_pid) *finder_pid = pid;
    if (application) {
        *application = AXUIElementCreateApplication(pid);
    }
    return true;
}

static CGFloat x_position(AXUIElementRef element) {
    CFTypeRef value = copy_attribute(element, kAXPositionAttribute);
    if (!value || CFGetTypeID(value) != AXValueGetTypeID()) {
        if (value) CFRelease(value);
        return 0;
    }

    CGPoint point = CGPointZero;
    AXValueGetValue((AXValueRef)value, kAXValueCGPointType, &point);
    CFRelease(value);
    return point.x;
}

static bool position_of(AXUIElementRef element, CGPoint *point) {
    CFTypeRef value = copy_attribute(element, kAXPositionAttribute);
    if (!value || CFGetTypeID(value) != AXValueGetTypeID()) {
        if (value) CFRelease(value);
        return false;
    }

    bool success = AXValueGetValue(
        (AXValueRef)value,
        kAXValueCGPointType,
        point
    );
    CFRelease(value);
    return success;
}

static bool descendant_selected(AXUIElementRef element, int depth) {
    if (bool_attribute(element, kAXSelectedAttribute)) return true;
    if (depth >= 4) return false;

    CFArrayRef children = copy_array_attribute(element, kAXChildrenAttribute);
    if (!children) return false;
    CFIndex count = CFArrayGetCount(children);
    bool selected = false;
    for (CFIndex index = 0; index < count && !selected; ++index) {
        selected = descendant_selected(
            (AXUIElementRef)CFArrayGetValueAtIndex(children, index),
            depth + 1
        );
    }
    CFRelease(children);
    return selected;
}

static CFIndex selected_count(
    AXUIElementRef container,
    navigation_role_t role,
    CFArrayRef items
) {
    CFStringRef selected_attribute = role == navigation_outline
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute;
    CFArrayRef selected = copy_array_attribute(container, selected_attribute);
    if (selected) {
        CFIndex count = CFArrayGetCount(selected);
        CFRelease(selected);
        return count;
    }

    // Compatibility fallback for Finder views that do not expose a selection
    // array. The common path above uses one AX request regardless of item count.
    if (role == navigation_list || role == navigation_grid) {
        CFIndex count = 0;
        CFIndex item_count = CFArrayGetCount(items);
        for (CFIndex index = 0; index < item_count; ++index) {
            AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
                items,
                index
            );
            if (descendant_selected(item, 0)) ++count;
        }
        return count;
    }

    CFIndex count = 0;
    CFIndex item_count = CFArrayGetCount(items);
    for (CFIndex index = 0; index < item_count; ++index) {
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(items, index);
        if (bool_attribute(item, kAXSelectedAttribute)) ++count;
    }
    return count;
}
