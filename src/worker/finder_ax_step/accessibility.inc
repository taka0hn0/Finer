static void state_path(char *buffer, size_t size, const char *name) {
    snprintf(buffer, size, "%s/.local/state/finder-vim/%s", home_directory(), name);
}

static CFTypeRef copy_attribute(AXUIElementRef element, CFStringRef name) {
    if (metrics_path) ++metrics_ax_reads;
    CFTypeRef value = NULL;
    if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess) {
        return NULL;
    }
    return value;
}

static AXError set_attribute(
    AXUIElementRef element,
    CFStringRef name,
    CFTypeRef value
) {
    if (metrics_path) ++metrics_ax_writes;
    return AXUIElementSetAttributeValue(element, name, value);
}

static bool role_of(AXUIElementRef element, navigation_role_t *role) {
    CFTypeRef value = copy_attribute(element, kAXRoleAttribute);
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) {
        if (value) CFRelease(value);
        return false;
    }

    bool supported = true;
    if (CFEqual(value, kAXOutlineRole)) {
        *role = navigation_outline;
    } else if (CFEqual(value, kAXListRole)) {
        CFTypeRef subrole = copy_attribute(element, kAXSubroleAttribute);
        bool collection_list = subrole
            && CFGetTypeID(subrole) == CFStringGetTypeID()
            && CFEqual(subrole, CFSTR("AXCollectionList"));
        if (subrole) CFRelease(subrole);
        *role = collection_list ? navigation_grid : navigation_list;
    } else if (CFEqual(value, kAXGridRole)) {
        *role = navigation_grid;
    } else {
        supported = false;
    }
    CFRelease(value);
    return supported;
}

static CFArrayRef copy_array_attribute(AXUIElementRef element, CFStringRef name) {
    CFTypeRef value = copy_attribute(element, name);
    if (!value || CFGetTypeID(value) != CFArrayGetTypeID()) {
        if (value) CFRelease(value);
        return NULL;
    }
    return (CFArrayRef)value;
}

static bool bool_attribute(AXUIElementRef element, CFStringRef name) {
    CFTypeRef value = copy_attribute(element, name);
    bool result = value && CFGetTypeID(value) == CFBooleanGetTypeID()
        && CFBooleanGetValue((CFBooleanRef)value);
    if (value) CFRelease(value);
    return result;
}

static CFArrayRef copy_navigation_items(
    AXUIElementRef container,
    navigation_role_t role
) {
    if (role == navigation_list) {
        return copy_array_attribute(container, kAXChildrenAttribute);
    }
    if (role == navigation_grid) {
        CFArrayRef sections = copy_array_attribute(
            container,
            kAXChildrenAttribute
        );
        if (!sections) return NULL;

        CFMutableArrayRef items = CFArrayCreateMutable(
            kCFAllocatorDefault,
            0,
            &kCFTypeArrayCallBacks
        );
        CFIndex section_count = CFArrayGetCount(sections);
        for (CFIndex index = 0; index < section_count; ++index) {
            AXUIElementRef section = (AXUIElementRef)CFArrayGetValueAtIndex(
                sections,
                index
            );
            CFArrayRef children = copy_array_attribute(
                section,
                kAXChildrenAttribute
            );
            if (!children) continue;
            CFIndex child_count = CFArrayGetCount(children);
            // Finder places non-file supplementary groups at the two edges of
            // a virtualized Icon section. Probe only those edges so the common
            // path does not add one AX request per visible file.
            CFIndex first_item = 0;
            while (first_item < child_count) {
                AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(
                    children,
                    first_item
                );
                CFStringRef url = copy_navigation_item_url_string(child, 0);
                if (url) {
                    CFRelease(url);
                    break;
                }
                ++first_item;
            }
            CFIndex item_end = child_count;
            while (item_end > first_item) {
                AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(
                    children,
                    item_end - 1
                );
                CFStringRef url = copy_navigation_item_url_string(child, 0);
                if (url) {
                    CFRelease(url);
                    break;
                }
                --item_end;
            }
            if (item_end > first_item) {
                CFArrayAppendArray(
                    items,
                    children,
                    CFRangeMake(first_item, item_end - first_item)
                );
            }
            CFRelease(children);
        }
        CFRelease(sections);
        return items;
    }
    return copy_array_attribute(container, kAXRowsAttribute);
}

static CFIndex raw_navigation_item_count(
    AXUIElementRef container,
    navigation_role_t role
) {
    CFStringRef attribute = role == navigation_outline
        ? kAXRowsAttribute
        : kAXChildrenAttribute;
    CFArrayRef items = copy_array_attribute(container, attribute);
    if (!items) return -1;
    CFIndex count = CFArrayGetCount(items);
    CFRelease(items);
    return count;
}

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
