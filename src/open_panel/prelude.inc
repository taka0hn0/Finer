#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const char *const open_panel_bundle_id = "com.microsoft.VSCode";
static const char *const open_panel_identifier = "open-panel";
static const int open_panel_fallback_interval_ms = 250;
static const int open_panel_max_session_seconds = 300;

typedef struct {
    pid_t pid;
    AXUIElementRef target;
    AXUIElementRef sheet;
} open_panel_context_t;

static volatile sig_atomic_t open_panel_should_stop;
static volatile sig_atomic_t open_panel_destroyed;
static bool open_panel_variables_active;
static int open_panel_socket_fd = -1;
static int open_panel_lock_fd = -1;
static char open_panel_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static uint64_t open_panel_monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000ULL
        + (uint64_t)value.tv_nsec / 1000000ULL;
}

static const char *open_panel_home_directory(void) {
    const char *home = getenv("HOME");
    return home && home[0] != '\0' ? home : "/tmp";
}

static bool open_panel_cf_string_equals(
    CFTypeRef value,
    CFStringRef expected
) {
    return value && CFGetTypeID(value) == CFStringGetTypeID()
        && CFStringCompare((CFStringRef)value, expected, 0)
            == kCFCompareEqualTo;
}

static void open_panel_release_context(open_panel_context_t *context) {
    if (!context) return;
    if (context->target) CFRelease(context->target);
    if (context->sheet) CFRelease(context->sheet);
    memset(context, 0, sizeof(*context));
}
