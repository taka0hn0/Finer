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

// Every AX action goes through here so the metrics counter cannot drift from
// the number of writes actually issued.
static AXError perform_ax_action(
    AXUIElementRef element,
    CFStringRef action
) {
    if (metrics_path) ++metrics_ax_writes;
    return AXUIElementPerformAction(element, action);
}

// Typed accessor for the many attributes that answer with another element.
// Callers get NULL instead of repeating the CFGetTypeID check and the release
// of an unexpected value.
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
