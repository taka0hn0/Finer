#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    direction_down,
    direction_up,
    direction_left,
    direction_right,
} direction_t;

typedef enum {
    navigation_outline,
    navigation_list,
    navigation_grid,
} navigation_role_t;

typedef struct {
    AXUIElementRef container;
    CFArrayRef items;
    CFArrayRef visible_marked_items;
    pid_t finder_pid;
    navigation_role_t role;
    CFIndex predicted_index;
    CFIndex cursor_index;
    bool has_marks;
    bool marks_loaded;
} navigation_context_t;

typedef struct {
    uint64_t submitted_nanoseconds;
    char command;
} worker_command_t;

typedef struct {
    uint64_t monotonic_nanoseconds;
    uint64_t ax_reads;
    uint64_t ax_writes;
    uint64_t cg_events;
    struct rusage_info_v4 usage;
    bool usage_available;
} metrics_snapshot_t;

typedef enum {
    column_transition_none = 0,
    column_transition_item_count = 1,
    column_transition_focused_container = 2,
    column_transition_timeout = 3,
} column_transition_reason_t;

typedef struct {
    uint64_t context_validation_nanoseconds;
    uint64_t context_creation_nanoseconds;
    uint64_t previous_item_count_nanoseconds;
    uint64_t movement_nanoseconds;
    uint64_t event_post_nanoseconds;
    uint64_t transition_total_nanoseconds;
    uint64_t transition_item_count_nanoseconds;
    uint64_t transition_focus_nanoseconds;
    uint64_t transition_candidate_items_nanoseconds;
    uint64_t transition_candidate_selection_nanoseconds;
    uint64_t transition_sleep_nanoseconds;
    uint64_t transition_attempts;
    column_transition_reason_t transition_reason;
} column_phase_metrics_t;

typedef struct {
    uint64_t timestamp_nanoseconds;
    uint64_t submitted_nanoseconds;
    uint64_t started_nanoseconds;
    uint64_t finished_nanoseconds;
    uint64_t user_cpu_nanoseconds;
    uint64_t system_cpu_nanoseconds;
    uint64_t package_idle_wakeups;
    uint64_t interrupt_wakeups;
    uint64_t resident_bytes;
    uint64_t physical_footprint_bytes;
    uint64_t ax_reads;
    uint64_t ax_writes;
    uint64_t cg_events;
    column_phase_metrics_t column_phases;
    CFIndex result_position;
    char command;
    bool cold;
    bool column_phase_metrics_enabled;
} metrics_record_t;

static int worker_idle_timeout_milliseconds = 750;
static const char *program_path;
static const char *metrics_path;
static const char *metrics_label;
static bool column_phase_metrics_enabled;
static uint64_t metrics_ax_reads;
static uint64_t metrics_ax_writes;
static uint64_t metrics_cg_events;
static metrics_record_t metrics_records[4096];
static size_t metrics_record_count;
static size_t metrics_dropped_records;
extern char **environ;

static uint64_t monotonic_nanoseconds(void);
static uint64_t counter_delta(uint64_t end, uint64_t start);
static CFStringRef copy_navigation_item_url_string(
    AXUIElementRef element,
    int depth
);

static const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') return home;

    struct passwd *password = getpwuid(getuid());
    return password ? password->pw_dir : "/tmp";
}

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
    return true;
}

static void navigation_context_release(navigation_context_t *context) {
    if (context->visible_marked_items) {
        CFRelease(context->visible_marked_items);
    }
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

static bool select_index(const navigation_context_t *context, CFIndex index) {
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

static int open_movement_lock(void) {
    char path[PATH_MAX];
    state_path(path, sizeof(path), "finder_ax_step.lock");
    return open(path, O_CREAT | O_RDWR, 0600);
}

static int open_worker_lock(void) {
    char path[PATH_MAX];
    state_path(path, sizeof(path), "finder_ax_step_worker.lock");
    return open(path, O_CREAT | O_RDWR, 0600);
}

static bool uses_descendant_selection(
    const navigation_context_t *context,
    CFIndex current
) {
    if ((context->role != navigation_list && context->role != navigation_grid)
        || current < 0) {
        return false;
    }

    CFArrayRef selected = copy_array_attribute(
        context->container,
        kAXSelectedChildrenAttribute
    );
    if (!selected || CFArrayGetCount(selected) == 0) {
        if (selected) CFRelease(selected);
        return true;
    }

    AXUIElementRef selected_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        selected,
        0
    );
    bool descendant = CFArrayGetFirstIndexOfValue(
        context->items,
        CFRangeMake(0, CFArrayGetCount(context->items)),
        selected_item
    ) == kCFNotFound;
    CFRelease(selected);
    return descendant;
}

static bool post_key_event(
    CGKeyCode key_code,
    bool key_down,
    bool autorepeat
) {
    CGEventRef event = CGEventCreateKeyboardEvent(NULL, key_code, key_down);
    if (!event) return false;
    if (autorepeat) {
        CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, 1);
    }
    CGEventPost(kCGHIDEventTap, event);
    if (metrics_path) ++metrics_cg_events;
    CFRelease(event);
    return true;
}

static bool post_key_code(CGKeyCode key_code) {
    bool key_down_posted = post_key_event(key_code, true, false);
    bool key_up_posted = post_key_event(key_code, false, false);
    return key_down_posted && key_up_posted;
}

static bool menu_item_has_deselect_shortcut(AXUIElementRef element) {
    CFTypeRef role = copy_attribute(element, kAXRoleAttribute);
    bool menu_item = role
        && CFGetTypeID(role) == CFStringGetTypeID()
        && CFEqual(role, kAXMenuItemRole);
    if (role) CFRelease(role);
    if (!menu_item) return false;

    CFTypeRef command_character = copy_attribute(
        element,
        kAXMenuItemCmdCharAttribute
    );
    bool command_is_a = command_character
        && CFGetTypeID(command_character) == CFStringGetTypeID()
        && CFStringCompare(
            (CFStringRef)command_character,
            CFSTR("A"),
            kCFCompareCaseInsensitive
        ) == kCFCompareEqualTo;
    if (command_character) CFRelease(command_character);

    if (!command_is_a) {
        CFTypeRef virtual_key = copy_attribute(
            element,
            kAXMenuItemCmdVirtualKeyAttribute
        );
        int32_t key_code = -1;
        command_is_a = virtual_key
            && CFGetTypeID(virtual_key) == CFNumberGetTypeID()
            && CFNumberGetValue(
                (CFNumberRef)virtual_key,
                kCFNumberSInt32Type,
                &key_code
            )
            && key_code == kVK_ANSI_A;
        if (virtual_key) CFRelease(virtual_key);
    }
    if (!command_is_a) return false;

    CFTypeRef modifier_value = copy_attribute(
        element,
        kAXMenuItemCmdModifiersAttribute
    );
    int32_t modifiers = 0;
    bool has_modifiers = modifier_value
        && CFGetTypeID(modifier_value) == CFNumberGetTypeID()
        && CFNumberGetValue(
            (CFNumberRef)modifier_value,
            kCFNumberSInt32Type,
            &modifiers
        );
    if (modifier_value) CFRelease(modifier_value);
    if (!has_modifiers) return false;

    return (modifiers & kAXMenuItemModifierOption) != 0
        && (modifiers & (
            kAXMenuItemModifierShift
            | kAXMenuItemModifierControl
            | kAXMenuItemModifierNoCommand
        )) == 0;
}

static AXUIElementRef copy_deselect_menu_item(
    AXUIElementRef element,
    int depth
) {
    if (menu_item_has_deselect_shortcut(element)) {
        return (AXUIElementRef)CFRetain(element);
    }
    if (depth >= 5) return NULL;

    CFArrayRef children = copy_array_attribute(element, kAXChildrenAttribute);
    if (!children) return NULL;
    AXUIElementRef result = NULL;
    CFIndex count = CFArrayGetCount(children);
    for (CFIndex index = 0; index < count && !result; ++index) {
        result = copy_deselect_menu_item(
            (AXUIElementRef)CFArrayGetValueAtIndex(children, index),
            depth + 1
        );
    }
    CFRelease(children);
    return result;
}

static bool perform_finder_deselect_menu_action(void) {
    AXUIElementRef application = NULL;
    if (!finder_is_frontmost(NULL, &application)) return false;

    CFTypeRef menu_bar_value = copy_attribute(application, kAXMenuBarAttribute);
    CFRelease(application);
    if (!menu_bar_value
        || CFGetTypeID(menu_bar_value) != AXUIElementGetTypeID()) {
        if (menu_bar_value) CFRelease(menu_bar_value);
        return false;
    }

    AXUIElementRef menu_item = copy_deselect_menu_item(
        (AXUIElementRef)menu_bar_value,
        0
    );
    CFRelease(menu_bar_value);
    if (!menu_item) return false;
    if (metrics_path) ++metrics_ax_writes;
    AXError error = AXUIElementPerformAction(menu_item, kAXPressAction);
    CFRelease(menu_item);
    return error == kAXErrorSuccess;
}

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
    double edge_value = last ? 1.0 : 0.0;
    CFNumberRef edge = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberDoubleType,
        &edge_value
    );
    bool success = edge && set_attribute(
        scroll_bar,
        kAXValueAttribute,
        edge
    ) == kAXErrorSuccess;
    if (edge) CFRelease(edge);
    CFRelease(scroll_bar);
    return success;
}

static bool scroll_outline_to_edge(
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

static CGKeyCode arrow_key_code(direction_t direction) {
    switch (direction) {
        case direction_down: return kVK_DownArrow;
        case direction_up: return kVK_UpArrow;
        case direction_left: return kVK_LeftArrow;
        case direction_right: return kVK_RightArrow;
    }
}

static bool arrow_key_is_down(direction_t direction) {
    return CGEventSourceKeyState(
        kCGEventSourceStateCombinedSessionState,
        arrow_key_code(direction)
    );
}

static CFIndex wait_for_selection_change(
    const navigation_context_t *context,
    CFIndex current
) {
    // Give Finder's main loop time to consume the posted key event before AX
    // polling. Immediate cross-process queries can otherwise keep returning the
    // old selection for the entire polling window.
    usleep(10000);
    for (int attempt = 0; attempt < 20; ++attempt) {
        CFIndex updated = current_index(context);
        if (updated >= 0 && updated != current) return updated;
        usleep(500);
    }
    return current;
}

static CFIndex select_outline_edge(
    const navigation_context_t *context,
    bool last
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex index = last ? count - 1 : 0;
    CFIndex step = last ? -1 : 1;
    for (; index >= 0 && index < count; index += step) {
        if (!select_index(context, index)) continue;
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (current_index(context) == index) return index + 1;
            usleep(500);
        }
    }
    return 0;
}

static CFIndex select_outline_next(
    const navigation_context_t *context,
    CFIndex current,
    direction_t direction
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex index = current;
    for (CFIndex attempt = 0; attempt < count; ++attempt) {
        if (direction == direction_down) {
            index = index < 0 ? 0 : (index + 1) % count;
        } else {
            index = index < 0 ? count - 1 : (index + count - 1) % count;
        }
        if (!select_index(context, index)) continue;
        for (int wait_attempt = 0; wait_attempt < 40; ++wait_attempt) {
            if (current_index(context) == index) return index + 1;
            usleep(500);
        }
    }
    return 0;
}

static CFIndex grid_column_index(
    const navigation_context_t *context,
    CFIndex index
) {
    AXUIElementRef target = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        index
    );
    CGPoint target_position;
    if (!position_of(target, &target_position)) return 0;

    CFIndex column = 0;
    CFIndex count = CFArrayGetCount(context->items);
    for (CFIndex item_index = 0; item_index < count; ++item_index) {
        CGPoint position;
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            item_index
        );
        if (position_of(item, &position)
            && fabs(position.y - target_position.y) <= 8.0
            && position.x < target_position.x) {
            ++column;
        }
    }
    return column;
}

static CFIndex grid_row_index(
    const navigation_context_t *context,
    CFIndex index
) {
    if (index <= 0) return 0;

    CGPoint previous_position;
    AXUIElementRef first_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        0
    );
    if (!position_of(first_item, &previous_position)) return 0;

    CFIndex row = 0;
    for (CFIndex item_index = 1; item_index <= index; ++item_index) {
        CGPoint position;
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            item_index
        );
        if (!position_of(item, &position)) continue;
        if (fabs(position.y - previous_position.y) > 8.0) ++row;
        previous_position = position;
    }
    return row;
}

static bool same_grid_row(
    const navigation_context_t *context,
    CFIndex first,
    CFIndex second
) {
    CGPoint first_position;
    CGPoint second_position;
    AXUIElementRef first_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        first
    );
    AXUIElementRef second_item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        second
    );
    return position_of(first_item, &first_position)
        && position_of(second_item, &second_position)
        && fabs(first_position.y - second_position.y) <= 8.0;
}

static CFIndex maximum_grid_column(
    const navigation_context_t *context
) {
    CFIndex maximum = 0;
    CFIndex count = CFArrayGetCount(context->items);
    for (CFIndex index = 0; index < count; ++index) {
        CFIndex column = grid_column_index(context, index);
        if (column > maximum) maximum = column;
    }
    return maximum;
}

static CFIndex select_grid_document_edge(
    navigation_context_t *context,
    bool last
) {
    CFIndex old_count = CFArrayGetCount(context->items);
    AXUIElementRef old_first = old_count > 0
        ? (AXUIElementRef)CFRetain(CFArrayGetValueAtIndex(context->items, 0))
        : NULL;
    AXUIElementRef old_last = old_count > 0
        ? (AXUIElementRef)CFRetain(
            CFArrayGetValueAtIndex(context->items, old_count - 1)
        )
        : NULL;
    if (!set_vertical_scroll_edge(context, last, NULL)) {
        if (old_last) CFRelease(old_last);
        if (old_first) CFRelease(old_first);
        return 0;
    }

    CFArrayRef replacement = NULL;
    for (unsigned attempt = 0; attempt < 80; ++attempt) {
        if (attempt == 0) usleep(5000);
        replacement = copy_navigation_items(
            context->container,
            context->role
        );
        if (replacement && CFArrayGetCount(replacement) > 0) {
            CFIndex replacement_count = CFArrayGetCount(replacement);
            AXUIElementRef replacement_first =
                (AXUIElementRef)CFArrayGetValueAtIndex(replacement, 0);
            AXUIElementRef replacement_last =
                (AXUIElementRef)CFArrayGetValueAtIndex(
                    replacement,
                    replacement_count - 1
                );
            bool changed = replacement_count != old_count
                || !old_first
                || !old_last
                || !CFEqual(old_first, replacement_first)
                || !CFEqual(old_last, replacement_last);
            if (changed || attempt == 79) break;
        }
        if (replacement) {
            CFRelease(replacement);
            replacement = NULL;
        }
        usleep(1000);
    }
    if (old_last) CFRelease(old_last);
    if (old_first) CFRelease(old_first);
    if (!replacement || CFArrayGetCount(replacement) == 0) {
        if (replacement) CFRelease(replacement);
        return 0;
    }

    CFRelease(context->items);
    context->items = replacement;
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex target = last ? count - 1 : 0;
    if (!select_index(context, target)) return 0;
    AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
        context->items,
        target
    );
    perform_ax_action(item, CFSTR("AXScrollToVisible"));
    context->predicted_index = target;
    return target + 1;
}

static CFIndex move_grid_horizontal(
    navigation_context_t *context,
    CFIndex current,
    direction_t direction,
    bool *navigation_may_change
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex target = direction == direction_right
        ? (current + 1) % count
        : (current + count - 1) % count;
    if (same_grid_row(context, current, target)) {
        if (!post_key_code(arrow_key_code(direction))) return 0;
        return target + 1;
    }

    double scroll_value = 0.0;
    bool has_scroll_value = vertical_scroll_value(context, &scroll_value);
    if (direction == direction_right) {
        if (target == 0) {
            if (has_scroll_value && scroll_value < 0.999) {
                if (!post_key_code(kVK_DownArrow)) return 0;
                CFIndex column = grid_column_index(context, current);
                for (CFIndex step = 0; step < column; ++step) {
                    post_key_code(kVK_LeftArrow);
                }
                wait_for_selection_change(context, current);
                if (navigation_may_change) *navigation_may_change = true;
                return current + 1;
            }
            if (has_scroll_value) {
                return select_grid_document_edge(context, false);
            }
            CFIndex row = grid_row_index(context, current);
            CFIndex column = grid_column_index(context, current);
            for (CFIndex step = 0; step < row; ++step) {
                post_key_code(kVK_UpArrow);
            }
            for (CFIndex step = 0; step < column; ++step) {
                post_key_code(kVK_LeftArrow);
            }
        } else {
            if (!post_key_code(kVK_DownArrow)) return 0;
            CFIndex column = grid_column_index(context, current);
            for (CFIndex step = 0; step < column; ++step) {
                post_key_code(kVK_LeftArrow);
            }
        }
    } else {
        if (current == 0) {
            if (has_scroll_value && scroll_value > 0.001) {
                if (!post_key_code(kVK_UpArrow)) return 0;
                CFIndex target_column = maximum_grid_column(context);
                for (CFIndex step = 0; step < target_column; ++step) {
                    post_key_code(kVK_RightArrow);
                }
                wait_for_selection_change(context, current);
                if (navigation_may_change) *navigation_may_change = true;
                return current + 1;
            }
            if (has_scroll_value) {
                return select_grid_document_edge(context, true);
            }
            CFIndex target_row = grid_row_index(context, target);
            CFIndex target_column = grid_column_index(context, target);
            for (CFIndex step = 0; step < target_row; ++step) {
                post_key_code(kVK_DownArrow);
            }
            for (CFIndex step = 0; step < target_column; ++step) {
                post_key_code(kVK_RightArrow);
            }
        } else {
            if (!post_key_code(kVK_UpArrow)) return 0;
            CFIndex target_column = grid_column_index(context, target);
            for (CFIndex step = 0; step < target_column; ++step) {
                post_key_code(kVK_RightArrow);
            }
        }
    }

    return target + 1;
}

static bool post_column_navigation_key(
    CGKeyCode key_code,
    column_phase_metrics_t *column_phases
) {
    if (!column_phases) return post_key_code(key_code);

    uint64_t started_nanoseconds = monotonic_nanoseconds();
    bool success = post_key_code(key_code);
    column_phases->event_post_nanoseconds += counter_delta(
        monotonic_nanoseconds(),
        started_nanoseconds
    );
    return success;
}

static CFIndex move_once(
    navigation_context_t *context,
    direction_t direction,
    int lock_fd,
    bool *navigation_may_change,
    bool use_navigation_anchor,
    column_phase_metrics_t *column_phases
) {
    if (navigation_may_change) *navigation_may_change = false;
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    if (use_navigation_anchor || !context->marks_loaded) {
        refresh_visible_marks(context);
    }

    CFIndex count = CFArrayGetCount(context->items);
    CFIndex anchor = context->cursor_index >= 0
        ? context->cursor_index
        : use_navigation_anchor
            ? take_navigation_anchor(context)
            : -1;
    if (anchor >= 0 && !context->has_marks
        && !select_index(context, anchor)) {
        anchor = -1;
    }
    if (anchor >= 0) context->predicted_index = -1;
    CFIndex current = anchor >= 0
        ? anchor
        : context->role == navigation_grid
            && context->predicted_index >= 0
        ? context->predicted_index
        : current_index(context);
    if (context->role == navigation_grid) {
        CFIndex position = 0;
        if (current >= 0
            && (direction == direction_left || direction == direction_right)) {
            if (context->has_marks) {
                CFIndex destination = direction == direction_right
                    ? (current + 1) % count
                    : (current + count - 1) % count;
                position = set_visible_mark_and_cursor_selection(
                    context,
                    destination
                ) ? destination + 1 : 0;
            } else {
                position = move_grid_horizontal(
                    context,
                    current,
                    direction,
                    navigation_may_change
                );
            }
        } else if (current >= 0 && post_key_code(arrow_key_code(direction))) {
            CFIndex destination = context->has_marks
                ? wait_for_selection_change(context, current)
                : current;
            position = destination >= 0 ? destination + 1 : 0;
        }
        if (context->has_marks && position > 0
            && direction != direction_left
            && direction != direction_right) {
            if (!set_visible_mark_and_cursor_selection(
                    context,
                    position - 1
                )) {
                position = 0;
            }
        }
        if (context->has_marks && position > 0) {
            context->cursor_index = position - 1;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return position;
    }

    if (context->role == navigation_outline
        && (direction == direction_down || direction == direction_up)) {
        CFIndex position = select_outline_next(context, current, direction);
        if (context->has_marks && position > 0
            && !set_visible_mark_and_cursor_selection(
                context,
                position - 1
            )) {
            position = 0;
        }
        if (context->has_marks && position > 0) {
            context->cursor_index = position - 1;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return position;
    }

    if (uses_descendant_selection(context, current)) {
        CFIndex position;
        if (direction == direction_left || direction == direction_right) {
            bool success = post_column_navigation_key(
                arrow_key_code(direction),
                column_phases
            );
            if (success && navigation_may_change) {
                *navigation_may_change = true;
            }
            position = success ? (current >= 0 ? current + 1 : 1) : 0;
        } else {
            bool success = post_column_navigation_key(
                arrow_key_code(direction),
                column_phases
            );
            CFIndex destination = success
                ? wait_for_selection_change(context, current)
                : -1;
            if (context->has_marks && destination >= 0
                && !set_visible_mark_and_cursor_selection(
                    context,
                    destination
                )) {
                destination = -1;
            }
            position = destination >= 0 ? destination + 1 : 0;
        }
        if (context->has_marks && position > 0) {
            context->cursor_index = position - 1;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return position;
    }

    if (direction == direction_left || direction == direction_right) {
        bool success = post_column_navigation_key(
            arrow_key_code(direction),
            column_phases
        );
        if (success && navigation_may_change) {
            *navigation_may_change = true;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return success ? (current >= 0 ? current + 1 : 1) : 0;
    }

    CFIndex destination;
    if (direction == direction_down) {
        destination = current < 0 ? 0 : (current + 1) % count;
    } else {
        destination = current < 0 ? count - 1 : (current + count - 1) % count;
    }

    bool success = set_visible_mark_and_cursor_selection(
        context,
        destination
    );
    if (success) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (context->has_marks || current_index(context) == destination) {
                break;
            }
            usleep(500);
        }
    }
    if (success && context->has_marks) {
        context->cursor_index = destination;
    }
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    return success ? destination + 1 : 0;
}

static CFIndex move_to_edge(
    navigation_context_t *context,
    bool last
) {
    refresh_visible_marks(context);
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex anchor = take_navigation_anchor(context);
    CFIndex current = anchor >= 0 ? anchor : current_index(context);
    if (context->has_marks && current >= 0 && !select_index(context, current)) {
        return 0;
    }
    if (context->role == navigation_grid
        || uses_descendant_selection(context, current)) {
        if (current < 0) return 0;
        AXUIElementRef current_item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            current
        );
        CGPoint current_position;
        if (!position_of(current_item, &current_position)) return 0;

        CFIndex target = current;
        CGFloat target_y = current_position.y;
        for (CFIndex index = 0; index < count; ++index) {
            AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
                context->items,
                index
            );
            CGPoint position;
            if (!position_of(item, &position)
                || fabs(position.x - current_position.x) > 8.0) {
                continue;
            }
            if ((last && position.y > target_y)
                || (!last && position.y < target_y)) {
                target = index;
                target_y = position.y;
            }
        }

        CFIndex current_row = grid_row_index(context, current);
        CFIndex target_row = grid_row_index(context, target);
        CFIndex steps = current_row > target_row
            ? current_row - target_row
            : target_row - current_row;
        CGKeyCode key_code = last ? kVK_DownArrow : kVK_UpArrow;
        for (CFIndex step = 0; step < steps; ++step) {
            if (!post_key_code(key_code)) return 0;
        }
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (current_index(context) == target) break;
            usleep(500);
        }
        CFIndex actual = current_index(context);
        if (actual < 0) return 0;
        if (context->has_marks
            && !set_visible_mark_and_cursor_selection(context, actual)) {
            return 0;
        }
        if (context->has_marks) context->cursor_index = actual;
        if (!write_navigation_anchor(context)) return 0;
        return actual + 1;
    }

    if (context->role == navigation_outline) {
        CFIndex position = select_outline_edge(context, last);
        if (position <= 0) return 0;
        if (context->has_marks
            && !set_visible_mark_and_cursor_selection(
                context,
                position - 1
            )) {
            return 0;
        }
        if (context->has_marks) context->cursor_index = position - 1;
        if (!scroll_outline_to_edge(context, position - 1, last)) return 0;
        if (!write_navigation_anchor(context)) return 0;
        return position;
    }

    CFIndex target = last ? count - 1 : 0;
    bool success = set_visible_mark_and_cursor_selection(context, target);
    if (success) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (context->has_marks || current_index(context) == target) break;
            usleep(500);
        }
    }
    if (success) {
        if (context->has_marks) context->cursor_index = target;
        success = write_navigation_anchor(context);
    }
    return success ? target + 1 : 0;
}

static void token_path(char *buffer, size_t size, direction_t direction) {
    const char *name;
    switch (direction) {
        case direction_down: name = "finder_down_hold.txt"; break;
        case direction_up: name = "finder_up_hold.txt"; break;
        case direction_left: name = "finder_left_hold.txt"; break;
        case direction_right: name = "finder_right_hold.txt"; break;
    }
    state_path(buffer, size, name);
}

static bool write_hold_token(direction_t direction) {
    char path[PATH_MAX];
    token_path(path, sizeof(path), direction);
    int descriptor = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (descriptor < 0) return false;

    uint64_t random_values[2];
    arc4random_buf(random_values, sizeof(random_values));
    char token[64];
    int length = snprintf(
        token,
        sizeof(token),
        "%016llx%016llx",
        random_values[0],
        random_values[1]
    );
    bool success = write(descriptor, token, (size_t)length) == length;
    close(descriptor);
    return success;
}

static ssize_t read_token(int descriptor, char *buffer, size_t size) {
    ssize_t count = pread(descriptor, buffer, size - 1, 0);
    if (count < 0) return count;
    buffer[count] = '\0';
    return count;
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static uint64_t monotonic_nanoseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static bool socket_address(struct sockaddr_un *address) {
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;

    char path[PATH_MAX];
    state_path(path, sizeof(path), "finder_ax_step.sock");
    if (strlen(path) >= sizeof(address->sun_path)) return false;
    snprintf(address->sun_path, sizeof(address->sun_path), "%s", path);
    return true;
}

static char direction_command(direction_t direction) {
    switch (direction) {
        case direction_down: return 'j';
        case direction_up: return 'k';
        case direction_left: return 'h';
        case direction_right: return 'l';
    }
}

static const char *direction_name(direction_t direction) {
    switch (direction) {
        case direction_down: return "down";
        case direction_up: return "up";
        case direction_left: return "left";
        case direction_right: return "right";
    }
}

static uint64_t realtime_nanoseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static bool valid_metrics_label(const char *label) {
    if (!label || label[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)label;
         *cursor;
         ++cursor) {
        bool valid = (*cursor >= 'a' && *cursor <= 'z')
            || (*cursor >= 'A' && *cursor <= 'Z')
            || (*cursor >= '0' && *cursor <= '9')
            || *cursor == '-'
            || *cursor == '_'
            || *cursor == '.';
        if (!valid) return false;
    }
    return true;
}

static bool supported_benchmark_idle_timeout(long milliseconds) {
    return milliseconds == 300
        || milliseconds == 500
        || milliseconds == 750
        || milliseconds == 1000
        || milliseconds == 1500;
}

static bool configure_metrics(void) {
    const char *path = getenv("FINDER_VIM_METRICS_FILE");
    metrics_path = path && path[0] != '\0' ? path : NULL;

    const char *label = getenv("FINDER_VIM_METRICS_LABEL");
    metrics_label = valid_metrics_label(label) ? label : "-";

    const char *column_phases = getenv("FINDER_VIM_COLUMN_PHASE_METRICS");
    column_phase_metrics_enabled = metrics_path
        && column_phases
        && strcmp(column_phases, "1") == 0;

    const char *timeout = getenv("FINDER_VIM_BENCHMARK_IDLE_TIMEOUT_MS");
    if (!metrics_path || !timeout || timeout[0] == '\0') return true;

    char *end = NULL;
    errno = 0;
    long milliseconds = strtol(timeout, &end, 10);
    if (errno != 0 || !end || *end != '\0'
        || !supported_benchmark_idle_timeout(milliseconds)) {
        fprintf(
            stderr,
            "finder_ax_step: unsupported benchmark idle timeout: %s\n",
            timeout
        );
        return false;
    }
    worker_idle_timeout_milliseconds = (int)milliseconds;
    return true;
}

static metrics_snapshot_t capture_metrics_snapshot(void) {
    metrics_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.monotonic_nanoseconds = monotonic_nanoseconds();
    snapshot.ax_reads = metrics_ax_reads;
    snapshot.ax_writes = metrics_ax_writes;
    snapshot.cg_events = metrics_cg_events;
    snapshot.usage_available = proc_pid_rusage(
        getpid(),
        RUSAGE_INFO_V4,
        (rusage_info_t *)&snapshot.usage
    ) == 0;
    return snapshot;
}

static uint64_t counter_delta(uint64_t end, uint64_t start) {
    return end >= start ? end - start : 0;
}

static void record_command_metrics(
    char command,
    bool cold,
    uint64_t submitted_nanoseconds,
    const metrics_snapshot_t *start,
    const metrics_snapshot_t *end,
    CFIndex result_position,
    const column_phase_metrics_t *column_phases
) {
    if (!metrics_path) return;
    if (metrics_record_count >= sizeof(metrics_records) / sizeof(metrics_records[0])) {
        ++metrics_dropped_records;
        return;
    }

    metrics_record_t *record = &metrics_records[metrics_record_count++];
    memset(record, 0, sizeof(*record));
    record->timestamp_nanoseconds = realtime_nanoseconds();
    record->submitted_nanoseconds = submitted_nanoseconds;
    record->started_nanoseconds = start->monotonic_nanoseconds;
    record->finished_nanoseconds = end->monotonic_nanoseconds;
    record->ax_reads = counter_delta(end->ax_reads, start->ax_reads);
    record->ax_writes = counter_delta(end->ax_writes, start->ax_writes);
    record->cg_events = counter_delta(end->cg_events, start->cg_events);
    if (column_phases) {
        record->column_phases = *column_phases;
        record->column_phase_metrics_enabled = true;
    }
    record->result_position = result_position;
    record->command = command;
    record->cold = cold;

    if (start->usage_available && end->usage_available) {
        record->user_cpu_nanoseconds = counter_delta(
            end->usage.ri_user_time,
            start->usage.ri_user_time
        );
        record->system_cpu_nanoseconds = counter_delta(
            end->usage.ri_system_time,
            start->usage.ri_system_time
        );
        record->package_idle_wakeups = counter_delta(
            end->usage.ri_pkg_idle_wkups,
            start->usage.ri_pkg_idle_wkups
        );
        record->interrupt_wakeups = counter_delta(
            end->usage.ri_interrupt_wkups,
            start->usage.ri_interrupt_wkups
        );
        record->resident_bytes = end->usage.ri_resident_size;
        record->physical_footprint_bytes = end->usage.ri_phys_footprint;
    }
}

static void flush_metrics(void) {
    if (!metrics_path || metrics_record_count == 0) return;

    int descriptor = open(metrics_path, O_CREAT | O_APPEND | O_WRONLY, 0600);
    if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0) {
        if (descriptor >= 0) close(descriptor);
        return;
    }

    struct stat status;
    if (fstat(descriptor, &status) == 0 && status.st_size == 0) {
        dprintf(
            descriptor,
            "timestamp_ns\tlabel\tpid\tworker_state\tcommand\t"
            "dispatch_to_selection_ns\tworker_duration_ns\tuser_cpu_ns\t"
            "system_cpu_ns\tpackage_idle_wakeups\tinterrupt_wakeups\t"
            "resident_bytes\tphysical_footprint_bytes\tax_reads\t"
            "ax_writes\tcg_events\tresult_position\tdropped_records\t"
            "worker_exit_after_command_ns\tcolumn_phase_metrics_enabled\t"
            "column_context_validation_ns\tcolumn_context_creation_ns\t"
            "column_previous_item_count_ns\tcolumn_movement_ns\t"
            "column_event_post_ns\tcolumn_transition_total_ns\t"
            "column_transition_item_count_ns\tcolumn_transition_focus_ns\t"
            "column_transition_candidate_items_ns\t"
            "column_transition_candidate_selection_ns\t"
            "column_transition_sleep_ns\tcolumn_transition_attempts\t"
            "column_transition_reason\n"
        );
    }

    pid_t pid = getpid();
    uint64_t flush_started_nanoseconds = monotonic_nanoseconds();
    for (size_t index = 0; index < metrics_record_count; ++index) {
        const metrics_record_t *record = &metrics_records[index];
        dprintf(
            descriptor,
            "%" PRIu64 "\t%s\t%d\t%s\t%c\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%ld\t%zu\t%" PRIu64 "\t%d"
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%d\n",
            record->timestamp_nanoseconds,
            metrics_label,
            pid,
            record->cold ? "cold" : "warm",
            record->command,
            counter_delta(
                record->finished_nanoseconds,
                record->submitted_nanoseconds
            ),
            counter_delta(
                record->finished_nanoseconds,
                record->started_nanoseconds
            ),
            record->user_cpu_nanoseconds,
            record->system_cpu_nanoseconds,
            record->package_idle_wakeups,
            record->interrupt_wakeups,
            record->resident_bytes,
            record->physical_footprint_bytes,
            record->ax_reads,
            record->ax_writes,
            record->cg_events,
            (long)record->result_position,
            metrics_dropped_records,
            counter_delta(
                flush_started_nanoseconds,
                record->finished_nanoseconds
            ),
            record->column_phase_metrics_enabled ? 1 : 0,
            record->column_phases.context_validation_nanoseconds,
            record->column_phases.context_creation_nanoseconds,
            record->column_phases.previous_item_count_nanoseconds,
            record->column_phases.movement_nanoseconds,
            record->column_phases.event_post_nanoseconds,
            record->column_phases.transition_total_nanoseconds,
            record->column_phases.transition_item_count_nanoseconds,
            record->column_phases.transition_focus_nanoseconds,
            record->column_phases.transition_candidate_items_nanoseconds,
            record->column_phases.transition_candidate_selection_nanoseconds,
            record->column_phases.transition_sleep_nanoseconds,
            record->column_phases.transition_attempts,
            record->column_phases.transition_reason
        );
    }

    flock(descriptor, LOCK_UN);
    close(descriptor);
}

static bool direction_from_command(char command, direction_t *direction) {
    switch (command) {
        case 'j': *direction = direction_down; return true;
        case 'k': *direction = direction_up; return true;
        case 'h': *direction = direction_left; return true;
        case 'l': *direction = direction_right; return true;
        default: return false;
    }
}

static bool send_worker_command(
    char command_value,
    uint64_t submitted_nanoseconds
) {
    struct sockaddr_un address;
    if (!socket_address(&address)) return false;

    int descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (descriptor < 0) return false;

    worker_command_t command;
    memset(&command, 0, sizeof(command));
    command.submitted_nanoseconds = submitted_nanoseconds;
    command.command = command_value;
    ssize_t sent = sendto(
        descriptor,
        &command,
        sizeof(command),
        0,
        (const struct sockaddr *)&address,
        sizeof(address)
    );
    close(descriptor);
    return sent == sizeof(command);
}

static bool run_toggle_mark_helper(void) {
    char helper_path[PATH_MAX];
    const char *override = getenv("FINDER_VIM_SWIFT_HELPER");
    if (override && override[0] != '\0') {
        if (snprintf(helper_path, sizeof(helper_path), "%s", override)
                >= (int)sizeof(helper_path)) {
            return false;
        }
    } else {
        const char *separator = strrchr(program_path, '/');
        if (!separator) return false;
        size_t directory_length = (size_t)(separator - program_path);
        const char helper_name[] = "/finder_ax_move";
        if (directory_length + sizeof(helper_name) > sizeof(helper_path)) {
            return false;
        }
        memcpy(helper_path, program_path, directory_length);
        memcpy(
            helper_path + directory_length,
            helper_name,
            sizeof(helper_name)
        );
    }

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDIN_FILENO,
        "/dev/null",
        O_RDONLY,
        0
    );
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDOUT_FILENO,
        "/dev/null",
        O_WRONLY,
        0
    );
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDERR_FILENO,
        "/dev/null",
        O_WRONLY,
        0
    );

    char *arguments[] = {
        helper_path,
        "toggle-mark",
        NULL,
    };
    pid_t child;
    int spawn_error = posix_spawn(
        &child,
        helper_path,
        &file_actions,
        NULL,
        arguments,
        environ
    );
    posix_spawn_file_actions_destroy(&file_actions);
    if (spawn_error != 0) return false;

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool focused_navigation_container_changed(
    AXUIElementRef previous_container,
    column_phase_metrics_t *column_phases
) {
    uint64_t focus_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    AXUIElementRef application = NULL;
    if (!finder_is_frontmost(NULL, &application)) {
        if (column_phases) {
            column_phases->transition_focus_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                focus_started_nanoseconds
            );
        }
        return false;
    }

    CFTypeRef focused = copy_attribute(application, kAXFocusedUIElementAttribute);
    CFRelease(application);
    if (!focused || CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        if (column_phases) {
            column_phases->transition_focus_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                focus_started_nanoseconds
            );
        }
        return false;
    }

    navigation_role_t role;
    AXUIElementRef current = copy_nearest_navigation_container(
        (AXUIElementRef)focused,
        &role
    );
    CFRelease(focused);
    if (column_phases) {
        column_phases->transition_focus_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            focus_started_nanoseconds
        );
    }
    if (!current) return false;
    bool changed = !CFEqual(current, previous_container);
    if (changed) {
        uint64_t items_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        CFArrayRef items = copy_navigation_items(current, role);
        if (column_phases) {
            column_phases->transition_candidate_items_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                items_started_nanoseconds
            );
        }
        navigation_context_t candidate = {
            .container = current,
            .items = items,
            .role = role,
        };
        if (items && CFArrayGetCount(items) > 0) {
            uint64_t selection_started_nanoseconds = column_phases
                ? monotonic_nanoseconds()
                : 0;
            changed = current_index(&candidate) >= 0;
            if (column_phases) {
                column_phases
                    ->transition_candidate_selection_nanoseconds += counter_delta(
                        monotonic_nanoseconds(),
                        selection_started_nanoseconds
                    );
            }
        } else {
            changed = false;
        }
        if (items) CFRelease(items);
    }
    CFRelease(current);
    return changed;
}

static void wait_for_navigation_transition(
    const navigation_context_t *context,
    CFIndex previous_item_count,
    column_phase_metrics_t *column_phases
) {
    // CGEventPost is asynchronous. Serialize the following Vim command behind
    // Finder's view update so it cannot mutate the previous directory.
    uint64_t transition_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    bool transitioned = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (column_phases) ++column_phases->transition_attempts;
        uint64_t item_count_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        CFIndex item_count = raw_navigation_item_count(
            context->container,
            context->role
        );
        if (column_phases) {
            column_phases->transition_item_count_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                item_count_started_nanoseconds
            );
        }
        if (previous_item_count >= 0 && item_count != previous_item_count) {
            if (column_phases) {
                column_phases->transition_reason = column_transition_item_count;
            }
            transitioned = true;
            break;
        }
        if (focused_navigation_container_changed(
                context->container,
                column_phases
            )) {
            if (column_phases) {
                column_phases->transition_reason =
                    column_transition_focused_container;
            }
            transitioned = true;
            break;
        }
        uint64_t sleep_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        usleep(2000);
        if (column_phases) {
            column_phases->transition_sleep_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                sleep_started_nanoseconds
            );
        }
    }
    if (column_phases) {
        if (!transitioned) {
            column_phases->transition_reason = column_transition_timeout;
        }
        column_phases->transition_total_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            transition_started_nanoseconds
        );
    }
}

static CFIndex move_worker_context(
    navigation_context_t *context,
    bool *context_created,
    direction_t direction,
    int movement_lock_fd,
    bool use_navigation_anchor,
    column_phase_metrics_t *column_phases
) {
    CFIndex previous_item_count = -1;
    if (direction == direction_left || direction == direction_right) {
        uint64_t item_count_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        previous_item_count = raw_navigation_item_count(
            context->container,
            context->role
        );
        if (column_phases) {
            column_phases->previous_item_count_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                item_count_started_nanoseconds
            );
        }
    }
    bool navigation_may_change = false;
    uint64_t movement_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    CFIndex position = move_once(
        context,
        direction,
        movement_lock_fd,
        &navigation_may_change,
        use_navigation_anchor,
        column_phases
    );
    if (column_phases) {
        column_phases->movement_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            movement_started_nanoseconds
        );
    }
    if (context->role == navigation_grid) {
        context->predicted_index = position > 0
                && (direction == direction_left || direction == direction_right)
            ? position - 1
            : -1;
    }
    if (navigation_may_change) {
        if (context->role != navigation_grid) {
            wait_for_navigation_transition(
                context,
                previous_item_count,
                column_phases
            );
        }
        navigation_context_release(context);
        *context_created = false;
    }
    return position;
}

static CFIndex worker_move(
    navigation_context_t *context,
    bool *context_created,
    direction_t direction,
    int movement_lock_fd,
    bool use_navigation_anchor,
    column_phase_metrics_t *column_phases
) {
    CFIndex validated_index = 0;
    if (*context_created) {
        uint64_t validation_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        validated_index = current_index(context);
        if (column_phases) {
            column_phases->context_validation_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                validation_started_nanoseconds
            );
        }
    }
    if (*context_created && validated_index < 0) {
        navigation_context_release(context);
        *context_created = false;
    }
    if (!*context_created) {
        uint64_t creation_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        *context_created = navigation_context_create(context);
        if (column_phases) {
            column_phases->context_creation_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                creation_started_nanoseconds
            );
        }
        if (!*context_created) return false;
    }

    CFIndex position = move_worker_context(
        context,
        context_created,
        direction,
        movement_lock_fd,
        use_navigation_anchor,
        column_phases
    );
    if (position > 0) return position;

    if (*context_created) navigation_context_release(context);
    uint64_t creation_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    *context_created = navigation_context_create(context);
    if (column_phases) {
        column_phases->context_creation_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            creation_started_nanoseconds
        );
    }
    if (!*context_created) return 0;
    return move_worker_context(
        context,
        context_created,
        direction,
        movement_lock_fd,
        false,
        column_phases
    );
}

static CFIndex execute_worker_command(
    navigation_context_t *context,
    bool *context_created,
    char command,
    int movement_lock_fd,
    bool cold,
    uint64_t submitted_nanoseconds
) {
    if (command == 'e') {
        metrics_snapshot_t start;
        if (metrics_path) start = capture_metrics_snapshot();
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_EX);
        bool success = clear_focused_selection_fallback();
        if (!success && !*context_created) {
            *context_created = navigation_context_create(context);
        }
        if (!success && *context_created) {
            success = clear_selection(context);
        }
        if (*context_created) {
            navigation_context_release(context);
            *context_created = false;
        }
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_UN);
        if (metrics_path) {
            metrics_snapshot_t end = capture_metrics_snapshot();
            record_command_metrics(
                command,
                cold,
                submitted_nanoseconds,
                &start,
                &end,
                success ? 1 : 0,
                NULL
            );
        }
        return success ? 1 : 0;
    }

    if (command == 's') {
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_EX);
        if (*context_created) {
            navigation_context_release(context);
            *context_created = false;
        }
        bool success = run_toggle_mark_helper();
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_UN);
        return success ? 1 : 0;
    }

    direction_t direction;
    if (!direction_from_command(command, &direction)) return 0;

    metrics_snapshot_t start;
    if (metrics_path) start = capture_metrics_snapshot();
    column_phase_metrics_t column_phases;
    column_phase_metrics_t *active_column_phases = NULL;
    if (column_phase_metrics_enabled) {
        memset(&column_phases, 0, sizeof(column_phases));
        active_column_phases = &column_phases;
    }
    CFIndex position = worker_move(
        context,
        context_created,
        direction,
        movement_lock_fd,
        true,
        active_column_phases
    );
    if (position > 0 && *context_created
        && context->has_marks
        && !write_navigation_anchor(context)) {
        position = 0;
    }
    if (metrics_path) {
        metrics_snapshot_t end = capture_metrics_snapshot();
        record_command_metrics(
            command,
            cold,
            submitted_nanoseconds,
            &start,
            &end,
            position,
            active_column_phases
        );
    }
    return position;
}

static int run_worker(
    char initial_command,
    int worker_lock_fd,
    uint64_t initial_submitted_nanoseconds
) {
    struct sockaddr_un address;
    if (!socket_address(&address)) {
        if (worker_lock_fd >= 0) close(worker_lock_fd);
        return 1;
    }

    unlink(address.sun_path);
    int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_fd < 0
        || bind(
            socket_fd,
            (const struct sockaddr *)&address,
            sizeof(address)
        ) != 0) {
        if (socket_fd >= 0) close(socket_fd);
        if (worker_lock_fd >= 0) close(worker_lock_fd);
        return 1;
    }
    chmod(address.sun_path, 0600);

    navigation_context_t context;
    memset(&context, 0, sizeof(context));
    bool context_created = false;
    int movement_lock_fd = open_movement_lock();

    CFIndex initial_result = 0;
    if (AXIsProcessTrusted() && finder_is_frontmost(NULL, NULL)) {
        initial_result = execute_worker_command(
            &context,
            &context_created,
            initial_command,
            movement_lock_fd,
            true,
            initial_submitted_nanoseconds
        );
    }
    if (initial_result == 0) {
        if (movement_lock_fd >= 0) close(movement_lock_fd);
        if (context_created) navigation_context_release(&context);
        close(socket_fd);
        unlink(address.sun_path);
        if (worker_lock_fd >= 0) {
            flock(worker_lock_fd, LOCK_UN);
            close(worker_lock_fd);
        }
        flush_metrics();
        return 1;
    }

    struct pollfd poll_descriptor = {
        .fd = socket_fd,
        .events = POLLIN,
        .revents = 0,
    };
    while (poll(&poll_descriptor, 1, worker_idle_timeout_milliseconds) > 0) {
        if (!(poll_descriptor.revents & POLLIN)) break;

        worker_command_t command;
        ssize_t received = recv(socket_fd, &command, sizeof(command), 0);
        if (received != sizeof(command)) continue;
        if (!finder_is_frontmost(NULL, NULL)) break;
        CFIndex result = execute_worker_command(
            &context,
            &context_created,
            command.command,
            movement_lock_fd,
            false,
            command.submitted_nanoseconds
        );
        if (result == 0) {
            break;
        }
    }

    if (movement_lock_fd >= 0) close(movement_lock_fd);
    if (context_created) navigation_context_release(&context);
    close(socket_fd);
    unlink(address.sun_path);
    if (worker_lock_fd >= 0) {
        flock(worker_lock_fd, LOCK_UN);
        close(worker_lock_fd);
    }
    flush_metrics();
    return 0;
}

static bool enqueue_worker_command(char command, const char *command_name) {
    uint64_t submitted_nanoseconds = monotonic_nanoseconds();
    int worker_lock_fd = open_worker_lock();
    if (worker_lock_fd < 0) return false;

    bool lock_acquired = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (flock(worker_lock_fd, LOCK_EX | LOCK_NB) == 0) {
            lock_acquired = true;
            break;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            close(worker_lock_fd);
            return false;
        }
        if (send_worker_command(command, submitted_nanoseconds)) {
            close(worker_lock_fd);
            return true;
        }
        usleep(500);
    }
    if (!lock_acquired) {
        close(worker_lock_fd);
        return false;
    }

    // A live worker owns this lock for its full lifetime. If we acquired it,
    // any existing socket has no receiver and must not be treated as a
    // successful enqueue target.
    struct sockaddr_un address;
    if (socket_address(&address)) {
        unlink(address.sun_path);
    }

    char lock_descriptor[32];
    char submitted_time[32];
    snprintf(lock_descriptor, sizeof(lock_descriptor), "%d", worker_lock_fd);
    snprintf(
        submitted_time,
        sizeof(submitted_time),
        "%llu",
        (unsigned long long)submitted_nanoseconds
    );
    char *worker_arguments[] = {
        (char *)program_path,
        "worker",
        (char *)command_name,
        lock_descriptor,
        submitted_time,
        NULL,
    };

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDIN_FILENO,
        "/dev/null",
        O_RDONLY,
        0
    );
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDOUT_FILENO,
        "/dev/null",
        O_WRONLY,
        0
    );
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDERR_FILENO,
        "/dev/null",
        O_WRONLY,
        0
    );

    pid_t child;
    int spawn_error = posix_spawn(
        &child,
        program_path,
        &file_actions,
        NULL,
        worker_arguments,
        environ
    );
    posix_spawn_file_actions_destroy(&file_actions);
    if (spawn_error != 0) {
        flock(worker_lock_fd, LOCK_UN);
        close(worker_lock_fd);
        return false;
    }

    close(worker_lock_fd);
    return true;
}

static bool enqueue_worker_step(direction_t direction) {
    return enqueue_worker_command(
        direction_command(direction),
        direction_name(direction)
    );
}

static bool enqueue_toggle_mark(void) {
    return enqueue_worker_command('s', "toggle-mark");
}

static bool enqueue_clear_selection(void) {
    return enqueue_worker_command('e', "clear-selection");
}

static int run_hold_start(direction_t direction) {
    bool enqueued = enqueue_worker_step(direction);
    bool token_written = write_hold_token(direction);
    return enqueued && token_written ? 0 : 1;
}

static bool hold_token_matches(
    int token_fd,
    const char *initial_token,
    ssize_t initial_length
) {
    char current_token[64];
    ssize_t current_length = read_token(
        token_fd,
        current_token,
        sizeof(current_token)
    );
    return current_length == initial_length
        && memcmp(initial_token, current_token, (size_t)initial_length) == 0;
}

static bool fast_ax_hold_mode_enabled(void) {
    const char *mode = getenv("FINDER_VIM_HOLD_MODE");
    return !mode || strcmp(mode, "ax-fast") == 0;
}

static useconds_t fast_ax_hold_interval_microseconds(void) {
    const useconds_t default_interval = 8333;
    const char *value = getenv("FINDER_VIM_HOLD_INTERVAL_US");
    if (!value || value[0] == '\0') return default_interval;

    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || end[0] != '\0'
        || parsed < 4000 || parsed > 50000) {
        return default_interval;
    }
    return (useconds_t)parsed;
}

static uint64_t mach_ticks_for_microseconds(useconds_t microseconds) {
    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase) != KERN_SUCCESS
        || timebase.numer == 0 || timebase.denom == 0) {
        return (uint64_t)microseconds * 1000ULL;
    }
    long double nanoseconds = (long double)microseconds * 1000.0L;
    return (uint64_t)(
        nanoseconds
            * (long double)timebase.denom
            / (long double)timebase.numer
    );
}

static bool mark_state_is_empty(void) {
    CFArrayRef urls = copy_mark_urls();
    bool empty = CFArrayGetCount(urls) == 0;
    CFRelease(urls);
    return empty;
}

static bool native_list_hold_enabled(void) {
    const char *value = getenv("FINDER_VIM_HOLD_LIST_NATIVE");
    return !value || strcmp(value, "0") != 0;
}

static bool supports_native_list_hold(
    const navigation_context_t *context,
    direction_t direction
) {
    return native_list_hold_enabled()
        && mark_state_is_empty()
        && (direction == direction_down || direction == direction_up)
        && context->role == navigation_outline;
}

static bool supports_fast_ax_hold(
    const navigation_context_t *context,
    direction_t direction
) {
    bool vertical_list = (direction == direction_down
            || direction == direction_up)
        && (context->role == navigation_outline
            || context->role == navigation_list);
    return fast_ax_hold_mode_enabled()
        && mark_state_is_empty()
        && vertical_list;
}

static bool direction_advances_index(direction_t direction) {
    return direction == direction_down || direction == direction_right;
}

static CFIndex fast_selected_index(
    const navigation_context_t *context
) {
    CFIndex item_count = CFArrayGetCount(context->items);
    CFStringRef selected_attribute = context->role == navigation_outline
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute;
    CFArrayRef selected = copy_array_attribute(
        context->container,
        selected_attribute
    );
    if (!selected || CFArrayGetCount(selected) == 0) {
        if (selected) CFRelease(selected);
        return -1;
    }

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
    return index == kCFNotFound ? -1 : index;
}

static CFIndex wait_for_fast_ax_index(
    const navigation_context_t *context,
    CFIndex expected
) {
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        CFIndex current = fast_selected_index(context);
        if (current == expected) return current + 1;
        usleep(500);
    }
    return 0;
}

static CFIndex recover_fast_ax_hold_position(
    const navigation_context_t *context,
    CFIndex predicted,
    direction_t direction
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex candidate = predicted;
    CFIndex attempt_limit = count < 8 ? count : 8;
    for (CFIndex attempt = 0; attempt < attempt_limit; ++attempt) {
        if (select_index(context, candidate)) {
            CFIndex position = wait_for_fast_ax_index(context, candidate);
            if (position > 0) return position;
        }
        candidate = direction_advances_index(direction)
            ? (candidate + 1) % count
            : (candidate + count - 1) % count;
    }
    return 0;
}

static CFIndex settle_fast_ax_hold_position(
    const navigation_context_t *context,
    CFIndex predicted,
    direction_t direction
) {
    CFIndex previous = -1;
    unsigned stable_samples = 0;
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        CFIndex current = fast_selected_index(context);
        if (current >= 0 && current == previous) {
            if (++stable_samples >= 2) return current + 1;
        } else {
            previous = current;
            stable_samples = 0;
        }
        usleep(1000);
    }
    if (previous >= 0) return previous + 1;

    // Group headers in Finder's outline can accept AXSelectedRows writes while
    // leaving the real selection empty. Keep that verification out of the hot
    // loop, then recover from the predicted row only when the hold stops.
    return recover_fast_ax_hold_position(context, predicted, direction);
}

static bool fast_ax_hold_scroll_to_visible_enabled(void) {
    const char *value = getenv("FINDER_VIM_HOLD_SCROLL_TO_VISIBLE");
    return !value || strcmp(value, "0") != 0;
}

typedef struct {
    AXUIElementRef scroll_bar;
    AXUIElementRef increment_button;
    AXUIElementRef decrement_button;
    CFNumberRef minimum_value;
    CFNumberRef maximum_value;
    CFIndex first_visible;
    CFIndex last_visible;
    bool enabled;
} fast_ax_scroll_state_t;

static void fast_ax_scroll_state_create(
    const navigation_context_t *context,
    fast_ax_scroll_state_t *state
) {
    memset(state, 0, sizeof(*state));
    state->first_visible = -1;
    state->last_visible = -1;
    state->enabled = fast_ax_hold_scroll_to_visible_enabled();
    if (!state->enabled) return;

    CFStringRef visible_attribute = context->role == navigation_outline
        ? kAXVisibleRowsAttribute
        : kAXVisibleChildrenAttribute;
    CFArrayRef visible = copy_array_attribute(
        context->container,
        visible_attribute
    );
    if (visible && CFArrayGetCount(visible) > 0) {
        CFIndex item_count = CFArrayGetCount(context->items);
        CFIndex visible_count = CFArrayGetCount(visible);
        state->first_visible = CFArrayGetFirstIndexOfValue(
            context->items,
            CFRangeMake(0, item_count),
            CFArrayGetValueAtIndex(visible, 0)
        );
        state->last_visible = CFArrayGetFirstIndexOfValue(
            context->items,
            CFRangeMake(0, item_count),
            CFArrayGetValueAtIndex(visible, visible_count - 1)
        );
        if (state->first_visible == kCFNotFound
            || state->last_visible == kCFNotFound) {
            state->first_visible = -1;
            state->last_visible = -1;
        }
    }
    if (visible) CFRelease(visible);

    if (context->role != navigation_outline) return;
    AXUIElementRef parent = copy_ax_element_attribute(
        context->container,
        kAXParentAttribute
    );
    if (!parent) return;
    state->scroll_bar = copy_ax_element_attribute(
        parent,
        kAXVerticalScrollBarAttribute
    );
    CFRelease(parent);
    if (!state->scroll_bar) return;
    state->increment_button = copy_ax_element_attribute(
        state->scroll_bar,
        kAXIncrementButtonAttribute
    );
    state->decrement_button = copy_ax_element_attribute(
        state->scroll_bar,
        kAXDecrementButtonAttribute
    );
    double minimum = 0.0;
    double maximum = 1.0;
    state->minimum_value = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberDoubleType,
        &minimum
    );
    state->maximum_value = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberDoubleType,
        &maximum
    );
}

static void fast_ax_scroll_state_release(fast_ax_scroll_state_t *state) {
    if (state->maximum_value) CFRelease(state->maximum_value);
    if (state->minimum_value) CFRelease(state->minimum_value);
    if (state->decrement_button) CFRelease(state->decrement_button);
    if (state->increment_button) CFRelease(state->increment_button);
    if (state->scroll_bar) CFRelease(state->scroll_bar);
    memset(state, 0, sizeof(*state));
}

static void scroll_fast_ax_target_to_visible(
    const navigation_context_t *context,
    fast_ax_scroll_state_t *state,
    CFIndex current,
    CFIndex target,
    direction_t direction,
    AXUIElementRef item
) {
    if (!state->enabled) return;

    CFIndex item_count = CFArrayGetCount(context->items);
    CFIndex visible_span = state->first_visible >= 0
        && state->last_visible >= state->first_visible
        ? state->last_visible - state->first_visible
        : 0;
    bool advances = direction_advances_index(direction);
    bool wrapped = (advances && target < current)
        || (!advances && target > current);
    if (wrapped && state->scroll_bar) {
        CFNumberRef edge = direction == direction_down
            ? state->minimum_value
            : state->maximum_value;
        if (!edge || set_attribute(
                state->scroll_bar,
                kAXValueAttribute,
                edge
            ) != kAXErrorSuccess) {
            perform_ax_action(item, CFSTR("AXScrollToVisible"));
            return;
        }
        if (direction == direction_down) {
            state->first_visible = 0;
            state->last_visible = visible_span;
        } else {
            state->last_visible = item_count - 1;
            state->first_visible = state->last_visible - visible_span;
        }
        return;
    }

    bool below = state->last_visible >= 0 && target > state->last_visible;
    bool above = state->first_visible >= 0 && target < state->first_visible;
    if (state->first_visible < 0 || state->last_visible < 0) {
        perform_ax_action(item, CFSTR("AXScrollToVisible"));
        return;
    }
    if (!below && !above) return;

    bool scrolled = false;
    if (context->role == navigation_outline) {
        AXUIElementRef button = below
            ? state->increment_button
            : state->decrement_button;
        CFIndex distance = below
            ? target - state->last_visible
            : state->first_visible - target;
        if (button) {
            scrolled = true;
            for (CFIndex step = 0; step < distance; ++step) {
                if (perform_ax_action(button, kAXPressAction)
                        != kAXErrorSuccess) {
                    scrolled = false;
                    break;
                }
            }
        }
    } else {
        scrolled = perform_ax_action(item, CFSTR("AXScrollToVisible"))
            == kAXErrorSuccess;
    }
    if (!scrolled) return;

    if (below) {
        CFIndex distance = target - state->last_visible;
        state->first_visible += distance;
        state->last_visible = target;
    } else {
        CFIndex distance = state->first_visible - target;
        state->first_visible = target;
        state->last_visible += distance;
    }
}

static CFIndex select_fast_ax_next(
    const navigation_context_t *context,
    CFIndex current,
    direction_t direction,
    CFMutableArrayRef selection,
    fast_ax_scroll_state_t *scroll_state
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex target = current;
    for (CFIndex attempt = 0; attempt < count; ++attempt) {
        target = direction_advances_index(direction)
            ? (target + 1) % count
            : (target + count - 1) % count;
        AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(
            context->items,
            target
        );
        if (CFArrayGetCount(selection) == 0) {
            CFArrayAppendValue(selection, item);
        } else {
            CFArraySetValueAtIndex(selection, 0, item);
        }

        AXError error;
        if (context->role == navigation_outline) {
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
        } else {
            error = set_attribute(
                context->container,
                kAXSelectedChildrenAttribute,
                selection
            );
        }
        if (error == kAXErrorSuccess) {
            scroll_fast_ax_target_to_visible(
                context,
                scroll_state,
                current,
                target,
                direction,
                item
            );
            return target;
        }
    }
    return -1;
}

static CFIndex run_fast_ax_hold_repeat(
    navigation_context_t *context,
    direction_t direction,
    int token_fd,
    const char *initial_token,
    ssize_t initial_length,
    double deadline,
    int lock_fd
) {
    CFIndex current = current_index(context);
    if (current < 0) return 0;

    CFMutableArrayRef selection = CFArrayCreateMutable(
        kCFAllocatorDefault,
        1,
        &kCFTypeArrayCallBacks
    );
    if (!selection) return 0;
    fast_ax_scroll_state_t scroll_state;
    fast_ax_scroll_state_create(context, &scroll_state);

    useconds_t interval = fast_ax_hold_interval_microseconds();
    uint64_t interval_ticks = mach_ticks_for_microseconds(interval);
    uint64_t next_step = mach_absolute_time();
    while (monotonic_seconds() < deadline) {
        uint64_t now = mach_absolute_time();
        if (now < next_step) {
            mach_wait_until(next_step);
        }
        if (monotonic_seconds() >= deadline
            || !hold_token_matches(
                token_fd,
                initial_token,
                initial_length
            )
            || !process_is_frontmost(context->finder_pid)) {
            break;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        CFIndex target = select_fast_ax_next(
            context,
            current,
            direction,
            selection,
            &scroll_state
        );
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        if (target < 0) break;
        current = target;

        next_step += interval_ticks;
        uint64_t finished = mach_absolute_time();
        if (finished >= next_step) {
            uint64_t missed = (finished - next_step) / interval_ticks + 1;
            next_step += missed * interval_ticks;
        }
    }

    CFIndex settled = 0;
    if (process_is_frontmost(context->finder_pid)) {
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        settled = settle_fast_ax_hold_position(
            context,
            current,
            direction
        );
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    }
    fast_ax_scroll_state_release(&scroll_state);
    CFRelease(selection);
    return settled;
}

static CFIndex run_native_list_hold_repeat(
    navigation_context_t *context,
    direction_t direction,
    int token_fd,
    const char *initial_token,
    ssize_t initial_length,
    double deadline,
    int lock_fd
) {
    CFIndex initial = current_index(context);
    if (initial < 0) return 0;

    CFIndex item_count = CFArrayGetCount(context->items);
    CFIndex predicted = initial;
    CFIndex posted_since_wrap = 0;
    // Keep the common native-repeat path free of AX readback. Only begin
    // probing after enough posted events could have reached the relevant
    // boundary, then require repeated stable observations before wrapping.
    CFIndex steps_before_probe = direction == direction_down
        ? item_count - initial - 1
        : initial;
    CFIndex posted_since_probe = 0;
    CFIndex last_observed = -1;
    unsigned stable_observations = 0;
    const CFIndex probe_interval_steps = 6;
    const unsigned stable_observations_before_wrap = 2;

    fast_ax_scroll_state_t scroll_state;
    memset(&scroll_state, 0, sizeof(scroll_state));
    bool scroll_state_created = false;

    useconds_t interval = fast_ax_hold_interval_microseconds();
    uint64_t interval_ticks = mach_ticks_for_microseconds(interval);
    uint64_t next_step = mach_absolute_time();
    CGKeyCode key_code = arrow_key_code(direction);
    bool key_is_down = false;

    while (monotonic_seconds() < deadline) {
        uint64_t now = mach_absolute_time();
        if (now < next_step) {
            mach_wait_until(next_step);
        }
        if (monotonic_seconds() >= deadline
            || !hold_token_matches(
                token_fd,
                initial_token,
                initial_length
            )
            || !process_is_frontmost(context->finder_pid)) {
            break;
        }

        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        bool posted = post_key_event(
            key_code,
            true,
            key_is_down
        );
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        if (!posted) break;
        key_is_down = true;

        if (item_count > 1) {
            ++posted_since_wrap;
            if (posted_since_wrap >= steps_before_probe
                && ++posted_since_probe >= probe_interval_steps) {
                posted_since_probe = 0;
                if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
                CFIndex observed = current_index(context);
                if (observed >= 0) predicted = observed;
                if (observed >= 0 && observed == last_observed) {
                    ++stable_observations;
                } else {
                    last_observed = observed;
                    stable_observations = 0;
                }

                bool should_wrap = stable_observations
                    >= stable_observations_before_wrap
                    && hold_token_matches(
                        token_fd,
                        initial_token,
                        initial_length
                    )
                    && process_is_frontmost(context->finder_pid);
                if (should_wrap) {
                    post_key_event(key_code, false, false);
                    key_is_down = false;
                    if (!scroll_state_created) {
                        fast_ax_scroll_state_create(context, &scroll_state);
                        scroll_state_created = true;
                    }
                    CFIndex wrapped_position = select_outline_edge(
                        context,
                        direction == direction_up
                    );
                    if (wrapped_position > 0) {
                        CFIndex target = wrapped_position - 1;
                        AXUIElementRef target_item =
                            (AXUIElementRef)CFArrayGetValueAtIndex(
                                context->items,
                                target
                            );
                        scroll_fast_ax_target_to_visible(
                            context,
                            &scroll_state,
                            observed,
                            target,
                            direction,
                            target_item
                        );
                        predicted = target;
                        posted_since_wrap = 0;
                        steps_before_probe = direction == direction_down
                            ? item_count - target - 1
                            : target;
                        posted_since_probe = 0;
                        last_observed = -1;
                        stable_observations = 0;
                    }
                }
                if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
            }
        }

        next_step += interval_ticks;
        uint64_t finished = mach_absolute_time();
        if (finished >= next_step) {
            uint64_t missed = (finished - next_step) / interval_ticks + 1;
            next_step += missed * interval_ticks;
        }
    }

    if (key_is_down) {
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        post_key_event(key_code, false, false);
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    }

    if (!process_is_frontmost(context->finder_pid)) {
        if (scroll_state_created) {
            fast_ax_scroll_state_release(&scroll_state);
        }
        return 0;
    }
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
    CFIndex settled = settle_fast_ax_hold_position(
        context,
        predicted,
        direction
    );
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    if (scroll_state_created) {
        fast_ax_scroll_state_release(&scroll_state);
    }
    return settled;
}

static int run_hold_repeat(direction_t direction) {
    char path[PATH_MAX];
    token_path(path, sizeof(path), direction);
    int token_fd = open(path, O_RDONLY);
    if (token_fd < 0) return 0;

    char initial_token[64];
    ssize_t initial_length = read_token(token_fd, initial_token, sizeof(initial_token));
    if (initial_length <= 0) {
        close(token_fd);
        return 0;
    }

    navigation_context_t context;
    int lock_fd = open_movement_lock();
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
    bool context_created = navigation_context_create(&context);
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    if (!context_created) {
        if (lock_fd >= 0) close(lock_fd);
        close(token_fd);
        return 1;
    }

    CFIndex last_position = 0;
    double deadline = monotonic_seconds() + 30.0;
    if (supports_native_list_hold(&context, direction)) {
        last_position = run_native_list_hold_repeat(
            &context,
            direction,
            token_fd,
            initial_token,
            initial_length,
            deadline,
            lock_fd
        );
    } else if (supports_fast_ax_hold(&context, direction)) {
        last_position = run_fast_ax_hold_repeat(
            &context,
            direction,
            token_fd,
            initial_token,
            initial_length,
            deadline,
            lock_fd
        );
    } else {
        unsigned iteration = 0;
        bool use_navigation_anchor = true;
        while (monotonic_seconds() < deadline) {
            if (!hold_token_matches(
                    token_fd,
                    initial_token,
                    initial_length
                )) {
                break;
            }
            if ((iteration++ % 20) == 0
                && !finder_is_frontmost(NULL, NULL)) {
                break;
            }

            last_position = worker_move(
                &context,
                &context_created,
                direction,
                lock_fd,
                use_navigation_anchor,
                NULL
            );
            use_navigation_anchor = false;
            if (last_position == 0) break;
            usleep(5000);
        }
    }

    if (last_position > 0 && context_created && context.has_marks
        && !write_navigation_anchor(&context)) {
        last_position = 0;
    }
    if (lock_fd >= 0) close(lock_fd);
    if (context_created) navigation_context_release(&context);
    close(token_fd);
    if (last_position > 0) printf("%ld\n", last_position);
    return 0;
}

static int open_list_edge_monitor_lock(direction_t direction) {
    char path[PATH_MAX];
    const char *name = direction == direction_down
        ? "finder_list_down_edge_monitor.lock"
        : "finder_list_up_edge_monitor.lock";
    state_path(path, sizeof(path), name);
    return open(path, O_CREAT | O_RDWR, 0600);
}

static int run_list_edge_monitor_worker(direction_t direction) {
    if (direction != direction_down && direction != direction_up) return 64;

    int monitor_lock_fd = open_list_edge_monitor_lock(direction);
    if (monitor_lock_fd < 0) return 1;
    if (flock(monitor_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(monitor_lock_fd);
        return errno == EWOULDBLOCK || errno == EAGAIN ? 0 : 1;
    }

    int result = 0;
    int movement_lock_fd = -1;
    navigation_context_t context;
    bool context_created = false;

    if (!AXIsProcessTrusted()) {
        result = 1;
        goto cleanup;
    }
    // The monitor is launched after Karabiner's delayed-action threshold. If
    // the remapped arrow is no longer down, the physical j/k was released and
    // there is no edge work to perform.
    if (!arrow_key_is_down(direction)) goto cleanup;

    movement_lock_fd = open_movement_lock();
    if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_EX);
    context_created = navigation_context_create(&context);
    if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_UN);
    if (!context_created) {
        result = 1;
        goto cleanup;
    }

    CFIndex item_count = CFArrayGetCount(context.items);
    if (context.role != navigation_outline
        || item_count < 2
        || !mark_state_is_empty()) {
        goto cleanup;
    }

    CFIndex boundary = direction == direction_down ? item_count - 1 : 0;
    CFIndex wrapped_target = direction == direction_down ? 0 : item_count - 1;
    CFIndex previous = -1;
    unsigned stable_observations = 0;
    bool awaiting_departure = false;
    double deadline = monotonic_seconds() + 30.0;

    while (monotonic_seconds() < deadline) {
        if (!arrow_key_is_down(direction)
            || !process_is_frontmost(context.finder_pid)) {
            break;
        }

        CFIndex current = current_index(&context);
        if (current < 0) {
            usleep(50000);
            continue;
        }

        if (awaiting_departure) {
            // Do not allow a one-item or stalled view to bounce repeatedly.
            // Native repeat must first move away from the wrapped edge.
            if (current != wrapped_target) awaiting_departure = false;
            previous = current;
            usleep(8333);
            continue;
        }

        if (current == boundary) {
            stable_observations = previous == current
                ? stable_observations + 1
                : 1;
            if (stable_observations >= 2) {
                if (!arrow_key_is_down(direction)
                    || !process_is_frontmost(context.finder_pid)
                    || !mark_state_is_empty()) {
                    break;
                }

                if (movement_lock_fd >= 0) {
                    flock(movement_lock_fd, LOCK_EX);
                }
                CFIndex confirmed = current_index(&context);
                CFIndex wrapped_position = 0;
                bool scrolled = false;
                if (confirmed == boundary
                    && arrow_key_is_down(direction)
                    && process_is_frontmost(context.finder_pid)) {
                    bool last = direction == direction_up;
                    wrapped_position = select_outline_edge(&context, last);
                    if (wrapped_position > 0) {
                        scrolled = scroll_outline_to_edge(
                            &context,
                            wrapped_position - 1,
                            last
                        );
                    }
                }
                if (movement_lock_fd >= 0) {
                    flock(movement_lock_fd, LOCK_UN);
                }
                if (wrapped_position == 0 || !scrolled) {
                    result = 1;
                    break;
                }

                awaiting_departure = true;
                stable_observations = 0;
                previous = wrapped_target;
                usleep(8333);
                continue;
            }
        } else {
            stable_observations = 0;
        }

        previous = current;
        CFIndex distance = direction == direction_down
            ? boundary - current
            : current;
        // Keep AX traffic low across the normal native-repeat path, then probe
        // at roughly one 120 Hz frame only when the selection is near an edge.
        usleep(distance <= 12 ? 8333 : 50000);
    }

cleanup:
    if (movement_lock_fd >= 0) close(movement_lock_fd);
    if (context_created) navigation_context_release(&context);
    flock(monitor_lock_fd, LOCK_UN);
    close(monitor_lock_fd);
    return result;
}

static bool spawn_list_edge_monitor(direction_t direction) {
    if (direction != direction_down && direction != direction_up) return false;

    const char *direction_value = direction == direction_down ? "down" : "up";
    char *arguments[] = {
        (char *)program_path,
        "list-edge-monitor-worker",
        (char *)direction_value,
        NULL,
    };

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDIN_FILENO,
        "/dev/null",
        O_RDONLY,
        0
    );
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDOUT_FILENO,
        "/dev/null",
        O_WRONLY,
        0
    );
    posix_spawn_file_actions_addopen(
        &file_actions,
        STDERR_FILENO,
        "/dev/null",
        O_WRONLY,
        0
    );

    pid_t child;
    int spawn_error = posix_spawn(
        &child,
        program_path,
        &file_actions,
        NULL,
        arguments,
        environ
    );
    posix_spawn_file_actions_destroy(&file_actions);
    return spawn_error == 0;
}

static bool parse_direction(const char *value, direction_t *direction) {
    if (strcmp(value, "down") == 0) {
        *direction = direction_down;
        return true;
    }
    if (strcmp(value, "up") == 0) {
        *direction = direction_up;
        return true;
    }
    if (strcmp(value, "left") == 0) {
        *direction = direction_left;
        return true;
    }
    if (strcmp(value, "right") == 0) {
        *direction = direction_right;
        return true;
    }
    return false;
}

int main(int argc, char **argv) {
    program_path = argv[0];
    if (!configure_metrics()) return 64;

    if (argc == 5 && strcmp(argv[1], "worker") == 0) {
        char worker_command;
        direction_t direction;
        if (strcmp(argv[2], "clear-selection") == 0) {
            worker_command = 'e';
        } else if (strcmp(argv[2], "toggle-mark") == 0) {
            worker_command = 's';
        } else if (parse_direction(argv[2], &direction)) {
            worker_command = direction_command(direction);
        } else {
            return 64;
        }
        char *end = NULL;
        long worker_lock_fd = strtol(argv[3], &end, 10);
        if (!end || *end != '\0' || worker_lock_fd < 0 || worker_lock_fd > INT_MAX) {
            return 64;
        }
        end = NULL;
        unsigned long long submitted_nanoseconds = strtoull(argv[4], &end, 10);
        if (!end || *end != '\0' || submitted_nanoseconds == 0) return 64;
        return run_worker(
            worker_command,
            (int)worker_lock_fd,
            submitted_nanoseconds
        );
    }

    if (argc == 2 && strcmp(argv[1], "toggle-mark") == 0) {
        return enqueue_toggle_mark() ? 0 : 1;
    }

    if (argc == 2 && strcmp(argv[1], "clear-selection") == 0) {
        return perform_finder_deselect_menu_action() || enqueue_clear_selection()
            ? 0
            : 1;
    }

    if (argc == 2 && (strcmp(argv[1], "first") == 0
            || strcmp(argv[1], "last") == 0)) {
        if (!AXIsProcessTrusted()) {
            fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
            return 1;
        }
        navigation_context_t context;
        int lock_fd = open_movement_lock();
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        if (!navigation_context_create(&context)) {
            if (lock_fd >= 0) close(lock_fd);
            return 1;
        }
        CFIndex position = move_to_edge(
            &context,
            strcmp(argv[1], "last") == 0
        );
        navigation_context_release(&context);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        if (position == 0) return 1;
        printf("%ld\n", position);
        return 0;
    }

    if (argc == 2 && (strcmp(argv[1], "down-wrap") == 0
            || strcmp(argv[1], "up-wrap") == 0
            || strcmp(argv[1], "left-wrap") == 0
            || strcmp(argv[1], "right-wrap") == 0)) {
        if (!AXIsProcessTrusted()) {
            fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
            return 1;
        }
        direction_t direction;
        if (strcmp(argv[1], "down-wrap") == 0) {
            direction = direction_down;
        } else if (strcmp(argv[1], "up-wrap") == 0) {
            direction = direction_up;
        } else if (strcmp(argv[1], "left-wrap") == 0) {
            direction = direction_left;
        } else {
            direction = direction_right;
        }
        navigation_context_t context;
        int lock_fd = open_movement_lock();
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        if (!navigation_context_create(&context)) {
            if (lock_fd >= 0) close(lock_fd);
            return 1;
        }
        CFIndex position = move_once(
            &context,
            direction,
            -1,
            NULL,
            true,
            NULL
        );
        navigation_context_release(&context);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        if (position == 0) return 1;
        printf("%ld\n", position);
        return 0;
    }

    if (argc == 3) {
        direction_t direction;
        if (!parse_direction(argv[2], &direction)) return 64;
        if (strcmp(argv[1], "list-edge-monitor-worker") == 0) {
            return run_list_edge_monitor_worker(direction);
        }
        if (strcmp(argv[1], "list-edge-monitor-start") == 0) {
            return spawn_list_edge_monitor(direction) ? 0 : 1;
        }
        if (strcmp(argv[1], "hold-start") == 0) return run_hold_start(direction);
        if (strcmp(argv[1], "hold-repeat") == 0) {
            if (!AXIsProcessTrusted()) {
                fprintf(stderr, "finder_ax_step: Accessibility access is unavailable\n");
                return 1;
            }
            return run_hold_repeat(direction);
        }
    }

    fprintf(
        stderr,
        "Usage: finder_ax_step <clear-selection|toggle-mark|first|last|down-wrap|up-wrap|left-wrap|right-wrap> | "
        "<hold-start|hold-repeat> <down|up|left|right> | "
        "list-edge-monitor-start <down|up>\n"
    );
    return 64;
}
