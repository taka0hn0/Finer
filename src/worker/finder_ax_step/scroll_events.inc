static AXUIElementRef copy_ax_element_attribute(
    AXUIElementRef element,
    CFStringRef attribute
) {
    CFTypeRef value = copy_attribute(element, attribute);
    if (!value || CFGetTypeID(value) != AXUIElementGetTypeID()) {
        if (value) CFRelease(value);
        return NULL;
    }
    return (AXUIElementRef)value;
}

static AXError perform_ax_action(
    AXUIElementRef element,
    CFStringRef action
) {
    if (metrics_path) ++metrics_ax_writes;
    return AXUIElementPerformAction(element, action);
}

static AXUIElementRef copy_vertical_scroll_bar(
    const navigation_context_t *context
) {
    AXUIElementRef current = (AXUIElementRef)CFRetain(context->container);
    for (int depth = 0; depth < 8; ++depth) {
        AXUIElementRef scroll_bar = copy_ax_element_attribute(
            current,
            kAXVerticalScrollBarAttribute
        );
        if (scroll_bar) {
            CFRelease(current);
            return scroll_bar;
        }

        AXUIElementRef parent = copy_ax_element_attribute(
            current,
            kAXParentAttribute
        );
        CFRelease(current);
        if (!parent) return NULL;
        current = parent;
    }
    CFRelease(current);
    return NULL;
}

static AXUIElementRef copy_scroll_bar_value_indicator(
    AXUIElementRef scroll_bar
) {
    CFArrayRef children = copy_array_attribute(
        scroll_bar,
        kAXChildrenAttribute
    );
    if (!children) return NULL;

    AXUIElementRef result = NULL;
    CFIndex count = CFArrayGetCount(children);
    for (CFIndex index = 0; index < count; ++index) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(
            children,
            index
        );
        CFTypeRef role = copy_attribute(child, kAXRoleAttribute);
        bool is_value_indicator = role
            && CFGetTypeID(role) == CFStringGetTypeID()
            && CFEqual(role, kAXValueIndicatorRole);
        if (role) CFRelease(role);
        if (!is_value_indicator) continue;
        result = (AXUIElementRef)CFRetain(child);
        break;
    }
    CFRelease(children);
    return result;
}

static bool vertical_scroll_value(
    const navigation_context_t *context,
    double *result
) {
    AXUIElementRef scroll_bar = copy_vertical_scroll_bar(context);
    if (!scroll_bar) return false;
    CFTypeRef value = copy_attribute(scroll_bar, kAXValueAttribute);
    CFRelease(scroll_bar);
    bool success = value && CFGetTypeID(value) == CFNumberGetTypeID()
        && CFNumberGetValue(
            (CFNumberRef)value,
            kCFNumberDoubleType,
            result
        );
    if (value) CFRelease(value);
    return success;
}

static bool set_vertical_scroll_edge(
    const navigation_context_t *context,
    bool last,
    bool *available
) {
    if (available) *available = false;
    AXUIElementRef scroll_bar = copy_vertical_scroll_bar(context);
    if (!scroll_bar) return false;
    if (available) *available = true;
    AXUIElementRef value_indicator = context->role == navigation_outline
        ? copy_scroll_bar_value_indicator(scroll_bar)
        : NULL;
    AXUIElementRef target = value_indicator
        ? value_indicator
        : scroll_bar;
    double double_edge = last ? 1.0 : 0.0;
    float float_edge = last ? 1.0f : 0.0f;
    CFNumberRef edge = CFNumberCreate(
        kCFAllocatorDefault,
        value_indicator ? kCFNumberFloatType : kCFNumberDoubleType,
        value_indicator
            ? (const void *)&float_edge
            : (const void *)&double_edge
    );
    bool success = edge
        && set_attribute(target, kAXValueAttribute, edge) == kAXErrorSuccess;
    if (!success && value_indicator && edge) {
        success = set_attribute(
            scroll_bar,
            kAXValueAttribute,
            edge
        ) == kAXErrorSuccess;
    }
    if (value_indicator) CFRelease(value_indicator);
    if (edge) CFRelease(edge);
    CFRelease(scroll_bar);
    return success;
}

static bool scroll_vertical_selection_to_edge(
    const navigation_context_t *context,
    CFIndex target,
    bool last
) {
    AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        target
    );
    bool scroll_bar_available = false;
    bool scrolled = set_vertical_scroll_edge(
        context,
        last,
        &scroll_bar_available
    );
    if (!scrolled) {
        scrolled = perform_ax_action(item, CFSTR("AXScrollToVisible"))
            == kAXErrorSuccess;
    }
    // A short List that already fits in its viewport has no vertical scroll
    // bar and Finder may not expose AXScrollToVisible on its rows. Selection
    // is already visible in that state, so there is no scrolling work to do.
    return scrolled || !scroll_bar_available;
}
