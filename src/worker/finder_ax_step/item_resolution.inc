static CFStringRef copy_navigation_item_url_string(
    AXUIElementRef element,
    int depth
) {
    CFTypeRef value = copy_attribute(element, kAXURLAttribute);
    if (value) {
        CFStringRef result = NULL;
        if (CFGetTypeID(value) == CFURLGetTypeID()) {
            CFURLRef absolute = CFURLCopyAbsoluteURL((CFURLRef)value);
            if (absolute) {
                result = CFStringCreateCopy(
                    kCFAllocatorDefault,
                    CFURLGetString(absolute)
                );
                CFRelease(absolute);
            }
        } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
            result = CFStringCreateCopy(
                kCFAllocatorDefault,
                (CFStringRef)value
            );
        }
        CFRelease(value);
        if (result) return result;
    }

    if (depth >= 3) return NULL;
    CFArrayRef children = copy_array_attribute(element, kAXChildrenAttribute);
    if (!children) return NULL;
    CFStringRef result = NULL;
    CFIndex count = CFArrayGetCount(children);
    for (CFIndex index = 0; index < count && !result; ++index) {
        result = copy_navigation_item_url_string(
            (AXUIElementRef)CFArrayGetValueAtIndex(children, index),
            depth + 1
        );
    }
    CFRelease(children);
    return result;
}

static bool navigation_item_is_selectable(
    const navigation_context_t *context,
    CFIndex index
) {
    CFIndex count = CFArrayGetCount(context->items);
    if (index < 0 || index >= count) return false;
    if (context->role == navigation_grid) return true;

    uint8_t cached = context->item_selectability
        ? context->item_selectability[index]
        : 0;
    if (cached != 0) return cached == 1;

    AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        index
    );
    CFStringRef url = copy_navigation_item_url_string(item, 0);
    bool selectable = url != NULL;
    if (url) CFRelease(url);
    if (context->item_selectability) {
        context->item_selectability[index] = selectable ? 1 : 2;
    }
    return selectable;
}

static bool parse_navigation_anchor_record(
    char *record,
    CFIndex *index_hint,
    CFStringRef *item_url
) {
    char *first_separator = strchr(record, '\t');
    if (!first_separator || first_separator == record
        || strncmp(record, "1", (size_t)(first_separator - record)) != 0) {
        return false;
    }

    char *index_text = first_separator + 1;
    char *second_separator = strchr(index_text, '\t');
    if (!second_separator || second_separator == index_text) return false;
    *second_separator = '\0';

    char *end = NULL;
    errno = 0;
    long parsed_index = strtol(index_text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed_index < 0) return false;

    char *url_text = second_separator + 1;
    size_t url_length = strlen(url_text);
    while (url_length > 0
            && (url_text[url_length - 1] == '\n'
                || url_text[url_length - 1] == '\r')) {
        url_text[--url_length] = '\0';
    }
    if (url_length == 0) return false;

    CFStringRef parsed_url = CFStringCreateWithCString(
        kCFAllocatorDefault,
        url_text,
        kCFStringEncodingUTF8
    );
    if (!parsed_url) return false;
    *index_hint = (CFIndex)parsed_index;
    *item_url = parsed_url;
    return true;
}

static bool navigation_item_matches_url(
    AXUIElementRef item,
    CFStringRef expected_url
) {
    CFStringRef item_url = copy_navigation_item_url_string(item, 0);
    if (!item_url) return false;

    bool matches = CFEqual(item_url, expected_url);
    if (!matches) {
        CFURLRef item_reference = CFURLCreateWithString(
            kCFAllocatorDefault,
            item_url,
            NULL
        );
        CFURLRef expected_reference = CFURLCreateWithString(
            kCFAllocatorDefault,
            expected_url,
            NULL
        );
        CFURLRef item_path_url = item_reference
            ? CFURLCreateFilePathURL(kCFAllocatorDefault, item_reference, NULL)
            : NULL;
        CFURLRef expected_path_url = expected_reference
            ? CFURLCreateFilePathURL(kCFAllocatorDefault, expected_reference, NULL)
            : NULL;
        CFStringRef item_path = item_path_url
            ? CFURLCopyFileSystemPath(item_path_url, kCFURLPOSIXPathStyle)
            : NULL;
        CFStringRef expected_path = expected_path_url
            ? CFURLCopyFileSystemPath(expected_path_url, kCFURLPOSIXPathStyle)
            : NULL;
        matches = item_path && expected_path && CFEqual(item_path, expected_path);

        if (item_path) CFRelease(item_path);
        if (expected_path) CFRelease(expected_path);
        if (item_path_url) CFRelease(item_path_url);
        if (expected_path_url) CFRelease(expected_path_url);
        if (item_reference) CFRelease(item_reference);
        if (expected_reference) CFRelease(expected_reference);
    }
    if (item_url) CFRelease(item_url);
    return matches;
}

static CFIndex navigation_item_index(
    const navigation_context_t *context,
    AXUIElementRef item
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex index = CFArrayGetFirstIndexOfValue(
        context->items,
        CFRangeMake(0, count),
        item
    );
    if (index != kCFNotFound || context->role == navigation_outline) {
        return index;
    }

    AXUIElementRef ancestor = copy_navigation_item_ancestor(
        item,
        context->items
    );
    if (!ancestor) return kCFNotFound;
    index = CFArrayGetFirstIndexOfValue(
        context->items,
        CFRangeMake(0, count),
        ancestor
    );
    CFRelease(ancestor);
    return index;
}

static CFIndex find_navigation_anchor_index(
    const navigation_context_t *context,
    CFIndex index_hint,
    CFStringRef item_url
) {
    CFIndex count = CFArrayGetCount(context->items);
    if (index_hint >= 0 && index_hint < count) {
        AXUIElementRef hinted_item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            index_hint
        );
        if (navigation_item_matches_url(hinted_item, item_url)) {
            return index_hint;
        }
    }

    CFStringRef selected_attribute = context->role == navigation_outline
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute;
    CFArrayRef selected = copy_array_attribute(
        context->container,
        selected_attribute
    );
    if (selected) {
        CFIndex selected_count = CFArrayGetCount(selected);
        for (CFIndex selected_index = 0;
                selected_index < selected_count;
                ++selected_index) {
            AXUIElementRef selected_item = (AXUIElementRef)CFArrayGetValueAtIndex(
                selected,
                selected_index
            );
            if (!navigation_item_matches_url(selected_item, item_url)) continue;
            CFIndex index = navigation_item_index(context, selected_item);
            CFRelease(selected);
            return index;
        }
        CFRelease(selected);
    }

    // The anchor can refer to an item that `s` just unmarked. This fallback is
    // intentionally limited to the one movement immediately following `s`.
    for (CFIndex index = 0; index < count; ++index) {
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            index
        );
        if (navigation_item_matches_url(item, item_url)) return index;
    }
    return kCFNotFound;
}

static CFIndex take_navigation_anchor(const navigation_context_t *context) {
    char path[PATH_MAX];
    navigation_anchor_path(path, sizeof(path));

    char claimed_path[PATH_MAX];
    int claimed_length = snprintf(
        claimed_path,
        sizeof(claimed_path),
        "%s.consuming.%ld",
        path,
        (long)getpid()
    );
    if (claimed_length < 0 || (size_t)claimed_length >= sizeof(claimed_path)) {
        return -1;
    }
    if (rename(path, claimed_path) != 0) return -1;

    int descriptor = open(claimed_path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        unlink(claimed_path);
        return -1;
    }
    char record[PATH_MAX * 4];
    ssize_t length = read(descriptor, record, sizeof(record) - 1);
    close(descriptor);
    unlink(claimed_path);
    if (length <= 0 || (size_t)length >= sizeof(record)) return -1;
    record[length] = '\0';

    CFIndex index_hint = -1;
    CFStringRef item_url = NULL;
    if (!parse_navigation_anchor_record(record, &index_hint, &item_url)) {
        return -1;
    }
    CFIndex index = find_navigation_anchor_index(
        context,
        index_hint,
        item_url
    );
    CFRelease(item_url);
    return index == kCFNotFound ? -1 : index;
}
