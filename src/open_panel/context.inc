static bool open_panel_frontmost_pid(pid_t *pid_out) {
    ProcessSerialNumber process_serial_number;
    pid_t pid = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus front_error = GetFrontProcess(&process_serial_number);
    OSStatus pid_error = front_error == noErr
        ? GetProcessPID(&process_serial_number, &pid)
        : front_error;
#pragma clang diagnostic pop
    if (front_error != noErr || pid_error != noErr || pid <= 0) {
        return false;
    }
    *pid_out = pid;
    return true;
}

static bool open_panel_pid_has_bundle_id(pid_t pid, const char *expected) {
    char executable_path[PROC_PIDPATHINFO_MAXSIZE];
    int length = proc_pidpath(pid, executable_path, sizeof(executable_path));
    if (length <= 0 || (size_t)length >= sizeof(executable_path)) return false;
    executable_path[length] = '\0';

    char *app_suffix = strstr(executable_path, ".app/");
    if (!app_suffix) return false;
    app_suffix[4] = '\0';

    CFURLRef bundle_url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        (const UInt8 *)executable_path,
        (CFIndex)strlen(executable_path),
        true
    );
    if (!bundle_url) return false;
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, bundle_url);
    CFRelease(bundle_url);
    if (!bundle) return false;

    CFStringRef expected_identifier = CFStringCreateWithCString(
        kCFAllocatorDefault,
        expected,
        kCFStringEncodingUTF8
    );
    CFStringRef identifier = CFBundleGetIdentifier(bundle);
    bool matches = identifier && expected_identifier && CFStringCompare(
        identifier,
        expected_identifier,
        0
    ) == kCFCompareEqualTo;
    if (expected_identifier) CFRelease(expected_identifier);
    CFRelease(bundle);
    return matches;
}

static CFTypeRef open_panel_copy_attribute(
    AXUIElementRef element,
    CFStringRef attribute
) {
    CFTypeRef value = NULL;
    if (!element || AXUIElementCopyAttributeValue(
        element,
        attribute,
        &value
    ) != kAXErrorSuccess) {
        return NULL;
    }
    return value;
}

static bool open_panel_context_model_accepts(
    const char *bundle_identifier,
    const char *sheet_identifier,
    const char *role,
    const char *subrole
) {
    (void)subrole;
    return bundle_identifier && sheet_identifier && role
        && strcmp(bundle_identifier, open_panel_bundle_id) == 0
        && strcmp(sheet_identifier, open_panel_identifier) == 0
        && (strcmp(role, "AXList") == 0 || strcmp(role, "AXOutline") == 0);
}

static bool open_panel_is_navigation_target(AXUIElementRef element) {
    CFTypeRef role = open_panel_copy_attribute(element, kAXRoleAttribute);
    bool accepted = open_panel_cf_string_equals(role, kAXListRole)
        || open_panel_cf_string_equals(role, kAXOutlineRole);
    if (role) CFRelease(role);
    return accepted;
}

static AXUIElementRef open_panel_copy_sheet_from_element(
    AXUIElementRef start
) {
    AXUIElementRef current = start ? (AXUIElementRef)CFRetain(start) : NULL;
    for (int depth = 0; current && depth < 16; ++depth) {
        CFTypeRef role = open_panel_copy_attribute(current, kAXRoleAttribute);
        bool is_sheet = open_panel_cf_string_equals(role, kAXSheetRole);
        if (role) CFRelease(role);
        if (is_sheet) {
            CFTypeRef identifier = open_panel_copy_attribute(
                current,
                kAXIdentifierAttribute
            );
            bool is_open_panel = open_panel_cf_string_equals(
                identifier,
                CFSTR("open-panel")
            );
            if (identifier) CFRelease(identifier);
            if (is_open_panel) return current;
        }

        CFTypeRef parent = open_panel_copy_attribute(current, kAXParentAttribute);
        CFRelease(current);
        current = parent && CFGetTypeID(parent) == AXUIElementGetTypeID()
            ? (AXUIElementRef)parent
            : NULL;
        if (parent && !current) CFRelease(parent);
    }
    if (current) CFRelease(current);
    return NULL;
}

static bool open_panel_copy_context(
    bool require_navigation_target,
    open_panel_context_t *context
) {
    memset(context, 0, sizeof(*context));
    if (!open_panel_frontmost_pid(&context->pid)
        || !open_panel_pid_has_bundle_id(context->pid, open_panel_bundle_id)) {
        return false;
    }

    AXUIElementRef application = AXUIElementCreateApplication(context->pid);
    if (!application) return false;
    CFTypeRef focused = open_panel_copy_attribute(
        application,
        kAXFocusedUIElementAttribute
    );
    CFRelease(application);
    if (!focused || CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        return false;
    }
    context->target = (AXUIElementRef)focused;
    if (require_navigation_target
        && !open_panel_is_navigation_target(context->target)) {
        open_panel_release_context(context);
        return false;
    }
    context->sheet = open_panel_copy_sheet_from_element(context->target);
    if (!context->sheet) {
        open_panel_release_context(context);
        return false;
    }
    return true;
}

static bool open_panel_session_context_is_valid(pid_t expected_pid) {
    pid_t frontmost_pid = 0;
    if (!open_panel_frontmost_pid(&frontmost_pid)
        || frontmost_pid != expected_pid) {
        return false;
    }
    AXUIElementRef application = AXUIElementCreateApplication(expected_pid);
    if (!application) return false;
    CFTypeRef focused = open_panel_copy_attribute(
        application,
        kAXFocusedUIElementAttribute
    );
    CFRelease(application);
    if (!focused || CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        return false;
    }
    AXUIElementRef sheet = open_panel_copy_sheet_from_element(
        (AXUIElementRef)focused
    );
    CFRelease(focused);
    if (!sheet) return false;
    CFRelease(sheet);
    return true;
}

static void open_panel_copy_string_attribute_to_buffer(
    AXUIElementRef element,
    CFStringRef attribute,
    char *buffer,
    size_t buffer_size
) {
    if (buffer_size == 0) return;
    buffer[0] = '\0';
    CFTypeRef value = open_panel_copy_attribute(element, attribute);
    if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
        (void)CFStringGetCString(
            (CFStringRef)value,
            buffer,
            (CFIndex)buffer_size,
            kCFStringEncodingUTF8
        );
    }
    if (value) CFRelease(value);
}

static int open_panel_print_diagnosis(void) {
    pid_t pid = 0;
    if (!open_panel_frontmost_pid(&pid)) {
        puts("{\"accepted\":false,\"reason\":\"no-frontmost-application\"}");
        return 1;
    }
    bool bundle_matches = open_panel_pid_has_bundle_id(
        pid,
        open_panel_bundle_id
    );
    AXUIElementRef application = AXUIElementCreateApplication(pid);
    CFTypeRef focused = application ? open_panel_copy_attribute(
        application,
        kAXFocusedUIElementAttribute
    ) : NULL;
    if (application) CFRelease(application);
    if (!focused || CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        printf(
            "{\"accepted\":false,\"pid\":%d,\"bundle_matches\":%s,\"reason\":\"no-focused-element\"}\n",
            pid,
            bundle_matches ? "true" : "false"
        );
        return 1;
    }

    char role[64];
    char subrole[64];
    open_panel_copy_string_attribute_to_buffer(
        (AXUIElementRef)focused,
        kAXRoleAttribute,
        role,
        sizeof(role)
    );
    open_panel_copy_string_attribute_to_buffer(
        (AXUIElementRef)focused,
        kAXSubroleAttribute,
        subrole,
        sizeof(subrole)
    );
    AXUIElementRef sheet = open_panel_copy_sheet_from_element(
        (AXUIElementRef)focused
    );
    bool accepted = bundle_matches && sheet
        && open_panel_is_navigation_target((AXUIElementRef)focused);
    printf(
        "{\"accepted\":%s,\"pid\":%d,\"bundle_matches\":%s,\"role\":\"%s\",\"subrole\":\"%s\",\"open_panel_sheet\":%s}\n",
        accepted ? "true" : "false",
        pid,
        bundle_matches ? "true" : "false",
        role,
        subrole,
        sheet ? "true" : "false"
    );
    if (sheet) CFRelease(sheet);
    CFRelease(focused);
    return accepted ? 0 : 1;
}
