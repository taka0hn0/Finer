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
    const char *override = getenv("KARABINER_FINDER_ANCHOR_FILE");
    if (override && override[0] != '\0') {
        snprintf(path, sizeof(path), "%s", override);
    } else {
        state_path(path, sizeof(path), "finder_navigation_anchor.txt");
    }

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

static void marks_path(char *buffer, size_t size) {
    const char *override = getenv("KARABINER_FINDER_MARKS_FILE");
    if (override && override[0] != '\0') {
        snprintf(buffer, size, "%s", override);
    } else {
        state_path(buffer, size, "finder_marks.txt");
    }
}

static CFArrayRef copy_mark_urls(void) {
    CFMutableArrayRef urls = CFArrayCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeArrayCallBacks
    );
    char path[PATH_MAX];
    marks_path(path, sizeof(path));
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) return urls;

    FILE *stream = fdopen(descriptor, "r");
    if (!stream) {
        close(descriptor);
        return urls;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        while (length > 0
                && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0) continue;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault,
            (const UInt8 *)line,
            length,
            false
        );
        if (!url) continue;
        CFStringRef url_string = CFStringCreateCopy(
            kCFAllocatorDefault,
            CFURLGetString(url)
        );
        CFRelease(url);
        if (!url_string) continue;
        CFArrayAppendValue(urls, url_string);
        CFRelease(url_string);
    }
    free(line);
    fclose(stream);
    return urls;
}

static bool item_matches_any_url(
    AXUIElementRef item,
    CFArrayRef urls
) {
    CFIndex count = CFArrayGetCount(urls);
    for (CFIndex index = 0; index < count; ++index) {
        if (navigation_item_matches_url(
                item,
                (CFStringRef)CFArrayGetValueAtIndex(urls, index)
            )) {
            return true;
        }
    }
    return false;
}

static CFArrayRef copy_selected_items(
    const navigation_context_t *context
) {
    CFStringRef attribute = context->role == navigation_outline
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute;
    CFArrayRef selected = copy_array_attribute(context->container, attribute);
    if (selected) return selected;

    CFMutableArrayRef fallback = CFArrayCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeArrayCallBacks
    );
    CFIndex count = CFArrayGetCount(context->items);
    for (CFIndex index = 0; index < count; ++index) {
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            index
        );
        bool selected_item = context->role == navigation_outline
            ? bool_attribute(item, kAXSelectedAttribute)
            : descendant_selected(item, 0);
        if (selected_item) CFArrayAppendValue(fallback, item);
    }
    return fallback;
}

static void refresh_visible_marks(navigation_context_t *context) {
    if (context->visible_marked_items) {
        CFRelease(context->visible_marked_items);
        context->visible_marked_items = NULL;
    }

    CFArrayRef mark_urls = copy_mark_urls();
    context->has_marks = CFArrayGetCount(mark_urls) > 0;
    context->marks_loaded = true;
    CFMutableArrayRef marked_items = CFArrayCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeArrayCallBacks
    );
    if (!context->has_marks) {
        context->cursor_index = -1;
        context->visible_marked_items = marked_items;
        CFRelease(mark_urls);
        return;
    }

    CFArrayRef selected = copy_selected_items(context);
    CFIndex selected_count = CFArrayGetCount(selected);
    for (CFIndex selected_index = 0;
            selected_index < selected_count;
            ++selected_index) {
        AXUIElementRef selected_item = (AXUIElementRef)CFArrayGetValueAtIndex(
            selected,
            selected_index
        );
        CFIndex item_index = navigation_item_index(context, selected_item);
        if (item_index == kCFNotFound) continue;
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            item_index
        );
        if (!item_matches_any_url(item, mark_urls)
            || CFArrayContainsValue(
                marked_items,
                CFRangeMake(0, CFArrayGetCount(marked_items)),
                item
            )) {
            continue;
        }
        CFArrayAppendValue(marked_items, item);
    }
    CFRelease(selected);
    CFRelease(mark_urls);
    context->visible_marked_items = marked_items;
}

static bool set_visible_mark_and_cursor_selection(
    const navigation_context_t *context,
    CFIndex cursor_index
) {
    if (!navigation_item_is_selectable(context, cursor_index)) return false;
    if (!context->has_marks) return select_index(context, cursor_index);

    CFMutableArrayRef selection = CFArrayCreateMutableCopy(
        kCFAllocatorDefault,
        0,
        context->visible_marked_items
    );
    AXUIElementRef cursor = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        cursor_index
    );
    if (!CFArrayContainsValue(
            selection,
            CFRangeMake(0, CFArrayGetCount(selection)),
            cursor
        )) {
        CFArrayAppendValue(selection, cursor);
    }

    CFStringRef attribute = context->role == navigation_outline
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute;
    AXError error = set_attribute(context->container, attribute, selection);
    CFRelease(selection);
    if (error == kAXErrorSuccess) {
        set_attribute(
            context->container,
            kAXFocusedAttribute,
            kCFBooleanTrue
        );
    }
    return error == kAXErrorSuccess;
}

static bool write_navigation_anchor(
    const navigation_context_t *context
) {
    if (!context->has_marks || context->cursor_index < 0
        || context->cursor_index >= CFArrayGetCount(context->items)) {
        return true;
    }

    AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        context->cursor_index
    );
    CFStringRef url = copy_navigation_item_url_string(item, 0);
    if (!url) return false;
    char url_text[PATH_MAX * 4];
    bool converted = CFStringGetCString(
        url,
        url_text,
        sizeof(url_text),
        kCFStringEncodingUTF8
    );
    CFRelease(url);
    if (!converted) return false;

    char path[PATH_MAX];
    const char *override = getenv("KARABINER_FINDER_ANCHOR_FILE");
    if (override && override[0] != '\0') {
        snprintf(path, sizeof(path), "%s", override);
    } else {
        state_path(path, sizeof(path), "finder_navigation_anchor.txt");
    }
    char temporary_path[PATH_MAX];
    int temporary_length = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp.%ld.%u",
        path,
        (long)getpid(),
        arc4random()
    );
    if (temporary_length < 0
        || (size_t)temporary_length >= sizeof(temporary_path)) {
        return false;
    }
    int descriptor = open(
        temporary_path,
        O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW,
        0600
    );
    if (descriptor < 0) return false;

    char record[PATH_MAX * 4 + 64];
    int record_length = snprintf(
        record,
        sizeof(record),
        "1\t%ld\t%s\n",
        context->cursor_index,
        url_text
    );
    bool success = record_length > 0
        && (size_t)record_length < sizeof(record)
        && write(descriptor, record, (size_t)record_length) == record_length;
    close(descriptor);
    if (success) success = rename(temporary_path, path) == 0;
    if (!success) unlink(temporary_path);
    return success;
}

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
