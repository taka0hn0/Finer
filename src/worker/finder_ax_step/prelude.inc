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

static void notify_keystroke(
    uint16_t key_code,
    uint64_t modifiers,
    int suppress_key_code,
    uint64_t suppress_modifiers
) {
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    int path_length = snprintf(
        address.sun_path,
        sizeof(address.sun_path),
        "/tmp/keystroke-finer-%u.sock",
        (unsigned)getuid()
    );
    if (path_length <= 0 || (size_t)path_length >= sizeof(address.sun_path)) {
        return;
    }
    if (access(address.sun_path, F_OK) != 0) return;

    int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_fd < 0) return;

    char message[96];
    int message_length = snprintf(
        message,
        sizeof(message),
        "%u %" PRIu64 " %d %" PRIu64 "\n",
        key_code,
        modifiers,
        suppress_key_code,
        suppress_modifiers
    );
    if (message_length > 0 && (size_t)message_length < sizeof(message)) {
        socklen_t address_length = (socklen_t)(
            offsetof(struct sockaddr_un, sun_path)
            + (size_t)path_length
            + 1
        );
        (void)sendto(
            socket_fd,
            message,
            (size_t)message_length,
            MSG_DONTWAIT,
            (const struct sockaddr *)&address,
            address_length
        );
    }
    close(socket_fd);
}

static uint16_t keystroke_key_code_for_direction(direction_t direction) {
    switch (direction) {
        case direction_down: return kVK_ANSI_J;
        case direction_up: return kVK_ANSI_K;
        case direction_left: return kVK_ANSI_H;
        case direction_right: return kVK_ANSI_L;
    }
}

static int keystroke_suppressed_key_code_for_direction(direction_t direction) {
    switch (direction) {
        case direction_down: return kVK_DownArrow;
        case direction_up: return kVK_UpArrow;
        case direction_left: return kVK_LeftArrow;
        case direction_right: return kVK_RightArrow;
    }
}

// Textual and transport spellings of a direction. These are CLI arguments,
// worker datagram payloads, and state file names rather than hot-path lookups,
// so they live together here and every fragment spells a direction one way.
// `arrow_key_code()` stays with the movement code: it is on the hold path and
// the compiler folds its switch into the caller.
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

static bool parse_direction(const char *value, direction_t *direction) {
    static const direction_t directions[] = {
        direction_down,
        direction_up,
        direction_left,
        direction_right,
    };
    if (!value) return false;
    for (size_t index = 0;
            index < sizeof(directions) / sizeof(directions[0]);
            ++index) {
        if (strcmp(value, direction_name(directions[index])) == 0) {
            *direction = directions[index];
            return true;
        }
    }
    return false;
}

typedef enum {
    navigation_outline,
    navigation_list,
    navigation_grid,
} navigation_role_t;

typedef enum {
    edge_monitor_list_view,
    edge_monitor_column_view,
} edge_monitor_view_t;

typedef struct {
    AXUIElementRef container;
    CFArrayRef items;
    CFArrayRef visible_marked_items;
    uint8_t *item_selectability;
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

// Types below are consumed by later fragments. They live here, with the rest
// of the shared types, so no fragment has to trail a definition that only its
// successor needs just to satisfy the fixed include order.

typedef struct {
    uint64_t interval_ticks;
    uint64_t next_step;
} hold_pacer_t;

typedef struct {
    AXUIElementRef scroll_bar;
    AXUIElementRef value_indicator;
    AXUIElementRef increment_button;
    AXUIElementRef decrement_button;
    CFNumberRef minimum_value;
    CFNumberRef maximum_value;
    CFIndex first_visible;
    CFIndex last_visible;
    bool enabled;
} fast_ax_scroll_state_t;

typedef enum {
    vertical_hold_stopped,
    vertical_hold_unchanged,
    vertical_hold_changed,
} vertical_hold_refresh_t;

typedef struct {
    int down_fd;
    int up_fd;
    direction_t direction;
    char token[64];
    ssize_t token_length;
} vertical_hold_state_t;

typedef struct {
    CFIndex previous;
    unsigned stable_observations;
} vertical_edge_monitor_state_t;

typedef struct {
    AXUIElementRef container;
    pid_t finder_pid;
} column_edge_context_t;

typedef struct {
    CFTypeRef previous;
} column_selection_stall_state_t;

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

#ifdef FINER_EDGE_TRACE
static int edge_trace_fd = -1;
#define EDGE_TRACE(...) do { \
    if (edge_trace_fd >= 0) dprintf(edge_trace_fd, __VA_ARGS__); \
} while (0)
#else
#define EDGE_TRACE(...) ((void)0)
#endif

static uint64_t monotonic_nanoseconds(void);
static uint64_t counter_delta(uint64_t end, uint64_t start);
static CFStringRef copy_navigation_item_url_string(
    AXUIElementRef element,
    int depth
);

// Column phase timing is opt-in through FINDER_VIM_COLUMN_PHASE_METRICS. When
// it is off every caller passes a NULL `column_phase_metrics_t *`, and neither
// helper below reads the clock, so ordinary movement keeps paying nothing for
// the instrumentation. Both stay in the header so movement_dispatch.inc, which
// is included before metrics.inc, can use them through the forward
// declarations above.
static uint64_t phase_timer_start(const column_phase_metrics_t *column_phases) {
    return column_phases ? monotonic_nanoseconds() : 0;
}

#define PHASE_TIMER_ADD(column_phases, field, started_nanoseconds) \
    do { \
        if (column_phases) { \
            (column_phases)->field += counter_delta( \
                monotonic_nanoseconds(), \
                (started_nanoseconds) \
            ); \
        } \
    } while (0)

static const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') return home;

    struct passwd *password = getpwuid(getuid());
    return password ? password->pw_dir : "/tmp";
}

// Runtime state lives in one directory, and the two files that tests relocate
// resolve their override in exactly one place. These are path helpers rather
// than Accessibility calls, so they sit with the process foundation.
static void state_path(char *buffer, size_t size, const char *name) {
    snprintf(buffer, size, "%s/.local/state/finder-vim/%s", home_directory(), name);
}

static void overridable_state_path(
    char *buffer,
    size_t size,
    const char *environment_variable,
    const char *name
) {
    const char *override = getenv(environment_variable);
    if (override && override[0] != '\0') {
        snprintf(buffer, size, "%s", override);
    } else {
        state_path(buffer, size, name);
    }
}

static void marks_path(char *buffer, size_t size) {
    overridable_state_path(
        buffer,
        size,
        "KARABINER_FINDER_MARKS_FILE",
        "finder_marks.txt"
    );
}

static void navigation_anchor_path(char *buffer, size_t size) {
    overridable_state_path(
        buffer,
        size,
        "KARABINER_FINDER_ANCHOR_FILE",
        "finder_navigation_anchor.txt"
    );
}
