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

static bool open_panel_copy_bundle_id(
    pid_t pid,
    char *buffer,
    size_t buffer_size
) {
    if (buffer_size == 0) return false;
    buffer[0] = '\0';

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

    CFStringRef identifier = CFBundleGetIdentifier(bundle);
    bool copied = identifier && CFStringGetCString(
        identifier,
        buffer,
        (CFIndex)buffer_size,
        kCFStringEncodingUTF8
    );
    CFRelease(bundle);
    return copied;
}

// The single allowlist decision for the helper. Both the production
// certification path and the self-test call this function, so a future change
// to the accepted applications or roles cannot pass the test while leaving the
// live behaviour untouched.
//
// The subrole is deliberately not part of the decision: Column exposes the
// navigation target as a plain AXList, while Icon exposes it as an AXList with
// the AXCollectionList subrole, and both are accepted.
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

// Nearest AXSheet ancestor, without judging its identifier. The caller reads
// the identifier and hands it to the allowlist above, so `open-panel` stays a
// single spelled-out constant.
static AXUIElementRef open_panel_copy_sheet_ancestor(AXUIElementRef start) {
    AXUIElementRef current = start ? (AXUIElementRef)CFRetain(start) : NULL;
    for (int depth = 0; current && depth < 16; ++depth) {
        CFTypeRef role = open_panel_copy_attribute(current, kAXRoleAttribute);
        bool is_sheet = open_panel_cf_string_equals(role, kAXSheetRole);
        if (role) CFRelease(role);
        if (is_sheet) return current;

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

typedef struct {
    char bundle_identifier[128];
    char sheet_identifier[64];
    char role[64];
    char subrole[64];
    bool has_sheet;
} open_panel_evaluation_t;

// Shared snapshot for certification, continued validation, and diagnosis.
// Each caller needs a different subset, so the snapshot is filled once and the
// decision itself is left to `open_panel_context_model_accepts`.
static void open_panel_evaluate(
    pid_t pid,
    AXUIElementRef target,
    AXUIElementRef sheet,
    open_panel_evaluation_t *evaluation
) {
    memset(evaluation, 0, sizeof(*evaluation));
    (void)open_panel_copy_bundle_id(
        pid,
        evaluation->bundle_identifier,
        sizeof(evaluation->bundle_identifier)
    );
    open_panel_copy_string_attribute_to_buffer(
        target,
        kAXRoleAttribute,
        evaluation->role,
        sizeof(evaluation->role)
    );
    open_panel_copy_string_attribute_to_buffer(
        target,
        kAXSubroleAttribute,
        evaluation->subrole,
        sizeof(evaluation->subrole)
    );
    evaluation->has_sheet = sheet != NULL;
    if (sheet) {
        open_panel_copy_string_attribute_to_buffer(
            sheet,
            kAXIdentifierAttribute,
            evaluation->sheet_identifier,
            sizeof(evaluation->sheet_identifier)
        );
    }
}

static bool open_panel_evaluation_accepts(
    const open_panel_evaluation_t *evaluation
) {
    return evaluation->has_sheet
        && open_panel_context_model_accepts(
            evaluation->bundle_identifier,
            evaluation->sheet_identifier,
            evaluation->role,
            evaluation->subrole
        );
}

// Copies the frontmost focused element and its nearest sheet ancestor without
// judging them. `open_panel_copy_context` certifies, the session loop revalidates,
// and `diagnose` reports, so the shared part stops at gathering AX state.
static bool open_panel_copy_focused_snapshot(open_panel_context_t *context) {
    memset(context, 0, sizeof(*context));
    if (!open_panel_frontmost_pid(&context->pid)) return false;

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
    context->sheet = open_panel_copy_sheet_ancestor(context->target);
    return true;
}

static bool open_panel_copy_context(
    bool require_navigation_target,
    open_panel_context_t *context
) {
    if (!open_panel_copy_focused_snapshot(context)) return false;

    open_panel_evaluation_t evaluation;
    open_panel_evaluate(
        context->pid,
        context->target,
        context->sheet,
        &evaluation
    );
    bool accepted = require_navigation_target
        ? open_panel_evaluation_accepts(&evaluation)
        : evaluation.has_sheet
            && strcmp(
                evaluation.bundle_identifier,
                open_panel_bundle_id
            ) == 0;
    if (!accepted) {
        open_panel_release_context(context);
        return false;
    }
    return true;
}

static bool open_panel_session_context_is_valid(pid_t expected_pid) {
    open_panel_context_t context;
    if (!open_panel_copy_focused_snapshot(&context)) return false;

    bool valid = context.pid == expected_pid && context.sheet != NULL;
    open_panel_release_context(&context);
    return valid;
}

static int open_panel_print_diagnosis(void) {
    open_panel_context_t context;
    if (!open_panel_copy_focused_snapshot(&context)) {
        pid_t pid = 0;
        if (!open_panel_frontmost_pid(&pid)) {
            puts("{\"accepted\":false,\"reason\":\"no-frontmost-application\"}");
            return 1;
        }
        char bundle_identifier[128];
        bool bundle_matches = open_panel_copy_bundle_id(
            pid,
            bundle_identifier,
            sizeof(bundle_identifier)
        ) && strcmp(bundle_identifier, open_panel_bundle_id) == 0;
        printf(
            "{\"accepted\":false,\"pid\":%d,\"bundle_matches\":%s,\"reason\":\"no-focused-element\"}\n",
            pid,
            bundle_matches ? "true" : "false"
        );
        return 1;
    }

    open_panel_evaluation_t evaluation;
    open_panel_evaluate(
        context.pid,
        context.target,
        context.sheet,
        &evaluation
    );
    bool accepted = open_panel_evaluation_accepts(&evaluation);
    printf(
        "{\"accepted\":%s,\"pid\":%d,\"bundle_matches\":%s,\"role\":\"%s\",\"subrole\":\"%s\",\"open_panel_sheet\":%s}\n",
        accepted ? "true" : "false",
        context.pid,
        strcmp(evaluation.bundle_identifier, open_panel_bundle_id) == 0
            ? "true"
            : "false",
        evaluation.role,
        evaluation.subrole,
        evaluation.has_sheet
            && strcmp(
                evaluation.sheet_identifier,
                open_panel_identifier
            ) == 0
            ? "true"
            : "false"
    );
    open_panel_release_context(&context);
    return accepted ? 0 : 1;
}
