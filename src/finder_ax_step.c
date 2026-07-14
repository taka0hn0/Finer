#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    navigation_role_t role;
} navigation_context_t;

static const int worker_idle_timeout_milliseconds = 750;
static const char *program_path;
extern char **environ;

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
    CFTypeRef value = NULL;
    if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess) {
        return NULL;
    }
    return value;
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
        *role = navigation_list;
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
    if (role == navigation_list || role == navigation_grid) {
        return copy_array_attribute(container, kAXChildrenAttribute);
    }

    CFArrayRef rows = copy_array_attribute(container, kAXRowsAttribute);
    if (!rows) return NULL;

    CFMutableArrayRef visible_rows = CFArrayCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeArrayCallBacks
    );
    CFIndex row_count = CFArrayGetCount(rows);
    for (CFIndex index = 0; index < row_count; ++index) {
        AXUIElementRef row = (AXUIElementRef)CFArrayGetValueAtIndex(rows, index);
        CFArrayRef cells = copy_array_attribute(row, kAXChildrenAttribute);
        if (!cells || CFArrayGetCount(cells) == 0) {
            if (cells) CFRelease(cells);
            continue;
        }

        AXUIElementRef first_cell = (AXUIElementRef)CFArrayGetValueAtIndex(cells, 0);
        CFArrayRef contents = copy_array_attribute(first_cell, kAXChildrenAttribute);
        if (contents && CFArrayGetCount(contents) > 0) {
            CFArrayAppendValue(visible_rows, row);
        }
        if (contents) CFRelease(contents);
        CFRelease(cells);
    }
    CFRelease(rows);
    return visible_rows;
}

static bool finder_is_frontmost(pid_t *finder_pid, AXUIElementRef *application) {
    ProcessSerialNumber process_serial_number;
    pid_t pid = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus front_process_error = GetFrontProcess(&process_serial_number);
    OSStatus process_pid_error = GetProcessPID(&process_serial_number, &pid);
#pragma clang diagnostic pop
    if (front_process_error != noErr || process_pid_error != noErr) return false;

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

static AXUIElementRef copy_navigation_container(
    AXUIElementRef focused,
    AXUIElementRef application,
    navigation_role_t *role
) {
    AXUIElementRef current = (AXUIElementRef)CFRetain(focused);
    for (int depth = 0; depth < 10; ++depth) {
        if (role_of(current, role)) {
            // The nearest supported ancestor is already the active Finder
            // view. Recursively rescanning its descendants makes a cold key
            // press proportional to the number of displayed files.
            return current;
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

    AXUIElementRef application = NULL;
    if (!finder_is_frontmost(NULL, &application)) {
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
    if (context->items) CFRelease(context->items);
    if (context->container) CFRelease(context->container);
    memset(context, 0, sizeof(*context));
}

static AXUIElementRef copy_direct_child_of_container(
    AXUIElementRef element,
    AXUIElementRef container
) {
    AXUIElementRef current = (AXUIElementRef)CFRetain(element);
    for (int depth = 0; depth < 8; ++depth) {
        CFTypeRef parent = copy_attribute(current, kAXParentAttribute);
        if (!parent || CFGetTypeID(parent) != AXUIElementGetTypeID()) {
            if (parent) CFRelease(parent);
            break;
        }
        if (CFEqual(parent, container)) {
            CFRelease(parent);
            return current;
        }
        CFRelease(current);
        current = (AXUIElementRef)parent;
    }
    CFRelease(current);
    return NULL;
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
            AXUIElementRef direct_child = copy_direct_child_of_container(
                selected_item,
                context->container
            );
            if (direct_child) {
                index = CFArrayGetFirstIndexOfValue(
                    context->items,
                    CFRangeMake(0, item_count),
                    direct_child
                );
                CFRelease(direct_child);
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
        error = AXUIElementSetAttributeValue(
            context->container,
            kAXSelectedChildrenAttribute,
            selection
        );
        AXUIElementSetAttributeValue(
            context->container,
            kAXFocusedAttribute,
            kCFBooleanTrue
        );
    } else {
        error = AXUIElementSetAttributeValue(
            context->container,
            kAXSelectedRowsAttribute,
            selection
        );
        if (error != kAXErrorSuccess) {
            CFIndex count = CFArrayGetCount(context->items);
            for (CFIndex row = 0; row < count; ++row) {
                AXUIElementRef row_item = (AXUIElementRef)CFArrayGetValueAtIndex(
                    context->items,
                    row
                );
                AXUIElementSetAttributeValue(
                    row_item,
                    kAXSelectedAttribute,
                    row == index ? kCFBooleanTrue : kCFBooleanFalse
                );
            }
            error = kAXErrorSuccess;
        }
    }
    CFRelease(selection);
    return error == kAXErrorSuccess;
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

static bool post_key_code(CGKeyCode key_code) {
    CGEventRef key_down = CGEventCreateKeyboardEvent(NULL, key_code, true);
    CGEventRef key_up = CGEventCreateKeyboardEvent(NULL, key_code, false);
    if (!key_down || !key_up) {
        if (key_down) CFRelease(key_down);
        if (key_up) CFRelease(key_up);
        return false;
    }

    CGEventPost(kCGHIDEventTap, key_down);
    CGEventPost(kCGHIDEventTap, key_up);
    CFRelease(key_down);
    CFRelease(key_up);
    return true;
}

static CGKeyCode arrow_key_code(direction_t direction) {
    switch (direction) {
        case direction_down: return kVK_DownArrow;
        case direction_up: return kVK_UpArrow;
        case direction_left: return kVK_LeftArrow;
        case direction_right: return kVK_RightArrow;
    }
}

static CFIndex wait_for_selection_change(
    const navigation_context_t *context,
    CFIndex current
) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        CFIndex updated = current_index(context);
        if (updated >= 0 && updated != current) return updated;
        usleep(500);
    }
    return current;
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

static CFIndex move_grid_horizontal(
    const navigation_context_t *context,
    CFIndex current,
    direction_t direction
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex target = direction == direction_right
        ? (current + 1) % count
        : (current + count - 1) % count;
    if (same_grid_row(context, current, target)) {
        if (!post_key_code(arrow_key_code(direction))) return 0;
        CFIndex moved = wait_for_selection_change(context, current);
        return moved >= 0 ? moved + 1 : 0;
    }

    if (direction == direction_right) {
        if (target == 0) {
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

    for (int attempt = 0; attempt < 40; ++attempt) {
        if (current_index(context) == target) return target + 1;
        usleep(500);
    }
    CFIndex actual = current_index(context);
    return actual >= 0 ? actual + 1 : 0;
}

static CFIndex move_once(
    const navigation_context_t *context,
    direction_t direction,
    int lock_fd
) {
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    CFIndex count = CFArrayGetCount(context->items);
    CFIndex current = current_index(context);
    if (uses_descendant_selection(context, current)) {
        CFIndex position;
        if (direction == direction_left || direction == direction_right) {
            position = move_grid_horizontal(context, current, direction);
        } else {
            bool success = post_key_code(arrow_key_code(direction));
            CFIndex destination = success
                ? wait_for_selection_change(context, current)
                : -1;
            position = destination >= 0 ? destination + 1 : 0;
        }
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return position;
    }

    if (direction == direction_left || direction == direction_right) {
        bool success = post_key_code(arrow_key_code(direction));
        if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
        return success ? (current >= 0 ? current + 1 : 1) : 0;
    }

    CFIndex destination;
    if (direction == direction_down) {
        destination = current < 0 ? 0 : (current + 1) % count;
    } else {
        destination = current < 0 ? count - 1 : (current + count - 1) % count;
    }

    bool success = select_index(context, destination);
    if (success) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (current_index(context) == destination) break;
            usleep(500);
        }
    }
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
    return success ? destination + 1 : 0;
}

static CFIndex move_to_edge(
    const navigation_context_t *context,
    bool last
) {
    CFIndex count = CFArrayGetCount(context->items);
    CFIndex current = current_index(context);
    if (uses_descendant_selection(context, current)) {
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
            if (current_index(context) == target) return target + 1;
            usleep(500);
        }
        CFIndex actual = current_index(context);
        return actual >= 0 ? actual + 1 : 0;
    }

    CFIndex target = last ? count - 1 : 0;
    bool success = select_index(context, target);
    if (success) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (current_index(context) == target) break;
            usleep(500);
        }
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

static bool direction_from_command(char command, direction_t *direction) {
    switch (command) {
        case 'j': *direction = direction_down; return true;
        case 'k': *direction = direction_up; return true;
        case 'h': *direction = direction_left; return true;
        case 'l': *direction = direction_right; return true;
        default: return false;
    }
}

static bool send_worker_step(direction_t direction) {
    struct sockaddr_un address;
    if (!socket_address(&address)) return false;

    int descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (descriptor < 0) return false;

    char command = direction_command(direction);
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

static CFIndex worker_move(
    navigation_context_t *context,
    bool *context_created,
    direction_t direction,
    int movement_lock_fd
) {
    if (*context_created && current_index(context) < 0) {
        navigation_context_release(context);
        *context_created = false;
    }
    if (!*context_created) {
        *context_created = navigation_context_create(context);
        if (!*context_created) return false;
    }

    CFIndex position = move_once(context, direction, movement_lock_fd);
    if (position > 0) return position;

    navigation_context_release(context);
    *context_created = navigation_context_create(context);
    if (!*context_created) return 0;
    return move_once(context, direction, movement_lock_fd);
}

static int run_worker(direction_t initial_direction, int worker_lock_fd) {
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

    if (!AXIsProcessTrusted()
        || !worker_move(
            &context,
            &context_created,
            initial_direction,
            movement_lock_fd
        )) {
        if (movement_lock_fd >= 0) close(movement_lock_fd);
        if (context_created) navigation_context_release(&context);
        close(socket_fd);
        unlink(address.sun_path);
        if (worker_lock_fd >= 0) {
            flock(worker_lock_fd, LOCK_UN);
            close(worker_lock_fd);
        }
        return 1;
    }

    struct pollfd poll_descriptor = {
        .fd = socket_fd,
        .events = POLLIN,
        .revents = 0,
    };
    while (poll(&poll_descriptor, 1, worker_idle_timeout_milliseconds) > 0) {
        if (!(poll_descriptor.revents & POLLIN)) break;

        char command;
        ssize_t received = recv(socket_fd, &command, sizeof(command), 0);
        if (received != sizeof(command)) continue;
        direction_t direction;
        if (!direction_from_command(command, &direction)) continue;
        if (!finder_is_frontmost(NULL, NULL)) break;
        if (!worker_move(
                &context,
                &context_created,
                direction,
                movement_lock_fd
            )) {
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
    return 0;
}

static bool enqueue_worker_step(direction_t direction) {
    if (send_worker_step(direction)) return true;

    int worker_lock_fd = open_worker_lock();
    if (worker_lock_fd < 0 || flock(worker_lock_fd, LOCK_EX) != 0) {
        if (worker_lock_fd >= 0) close(worker_lock_fd);
        return false;
    }

    if (send_worker_step(direction)) {
        flock(worker_lock_fd, LOCK_UN);
        close(worker_lock_fd);
        return true;
    }

    char lock_descriptor[32];
    snprintf(lock_descriptor, sizeof(lock_descriptor), "%d", worker_lock_fd);
    char *worker_arguments[] = {
        (char *)program_path,
        "worker",
        (char *)direction_name(direction),
        lock_descriptor,
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

static int run_hold_start(direction_t direction) {
    bool enqueued = enqueue_worker_step(direction);
    bool token_written = write_hold_token(direction);
    return enqueued && token_written ? 0 : 1;
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
    unsigned iteration = 0;
    while (monotonic_seconds() < deadline) {
        char current_token[64];
        ssize_t current_length = read_token(token_fd, current_token, sizeof(current_token));
        if (current_length != initial_length
            || memcmp(initial_token, current_token, (size_t)initial_length) != 0) {
            break;
        }
        if ((iteration++ % 20) == 0 && !finder_is_frontmost(NULL, NULL)) break;

        last_position = worker_move(
            &context,
            &context_created,
            direction,
            lock_fd
        );
        if (last_position == 0) break;
        usleep(5000);
    }

    if (lock_fd >= 0) close(lock_fd);
    if (context_created) navigation_context_release(&context);
    close(token_fd);
    if (last_position > 0) printf("%ld\n", last_position);
    return 0;
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

    if (argc == 4 && strcmp(argv[1], "worker") == 0) {
        direction_t direction;
        if (!parse_direction(argv[2], &direction)) return 64;
        char *end = NULL;
        long worker_lock_fd = strtol(argv[3], &end, 10);
        if (!end || *end != '\0' || worker_lock_fd < 0 || worker_lock_fd > INT_MAX) {
            return 64;
        }
        return run_worker(direction, (int)worker_lock_fd);
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
        CFIndex position = move_once(&context, direction, -1);
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
        "Usage: finder_ax_step <first|last|down-wrap|up-wrap|left-wrap|right-wrap> | "
        "<hold-start|hold-repeat> <down|up|left|right>\n"
    );
    return 64;
}
