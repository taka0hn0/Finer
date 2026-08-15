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
