#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

static NSPanel *markerPanel;
static dispatch_group_t childProcesses;
static volatile int exitStatus;

static int parseInteger(const char *value, const char *name, int minimum, int maximum) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum) {
        fprintf(stderr, "Invalid %s: %s\n", name, value);
        exit(64);
    }
    return (int)parsed;
}

static int environmentInteger(const char *name, int fallback, int minimum, int maximum) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    return parseInteger(value, name, minimum, maximum);
}

static void recordFailure(int status) {
    if (status != 0) {
        __sync_bool_compare_and_swap(&exitStatus, 0, status);
    }
}

static void waitForChild(pid_t pid) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        int status = 0;
        pid_t result;
        do {
            result = waitpid(pid, &status, 0);
        } while (result == -1 && errno == EINTR);

        if (result == -1) {
            recordFailure(1);
        } else if (!WIFEXITED(status)) {
            recordFailure(1);
        } else {
            recordFailure(WEXITSTATUS(status));
        }
        dispatch_group_leave(childProcesses);
    });
}

static void spawnTracked(const char *path, char *const argv[]) {
    pid_t pid = 0;
    int result = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (result != 0) {
        fprintf(stderr, "posix_spawn failed for %s: %s\n", path, strerror(result));
        recordFailure(1);
        dispatch_group_leave(childProcesses);
        return;
    }
    waitForChild(pid);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 12) {
            fprintf(stderr,
                    "Usage: %s OUTPUT RECT_X RECT_Y RECT_WIDTH RECT_HEIGHT "
                    "MARKER_X MARKER_Y MARKER_SIZE HELPER COMMAND DIRECTION\n",
                    argv[0]);
            return 64;
        }

        int rectX = parseInteger(argv[2], "RECT_X", -100000, 100000);
        int rectY = parseInteger(argv[3], "RECT_Y", -100000, 100000);
        int rectWidth = parseInteger(argv[4], "RECT_WIDTH", 100, 100000);
        int rectHeight = parseInteger(argv[5], "RECT_HEIGHT", 100, 100000);
        int markerX = parseInteger(argv[6], "MARKER_X", -100000, 100000);
        int markerY = parseInteger(argv[7], "MARKER_Y", -100000, 100000);
        int markerSize = parseInteger(argv[8], "MARKER_SIZE", 8, 200);
        int captureSeconds = environmentInteger(
            "FINER_VISUAL_CAPTURE_SECONDS", 2, 1, 10);
        int markerDelayMs = environmentInteger(
            "FINER_VISUAL_MARKER_DELAY_MS", 800, 100, 5000);

        if (markerX < rectX || markerY < rectY
            || markerX + markerSize > rectX + rectWidth
            || markerY + markerSize > rectY + rectHeight) {
            fprintf(stderr, "Marker must be fully inside the capture rectangle\n");
            return 64;
        }

        NSApplication *application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyProhibited];

        CGRect displayBounds = CGDisplayBounds(CGMainDisplayID());
        CGFloat appKitY = CGRectGetMaxY(displayBounds) - markerY - markerSize;
        NSRect markerRect = NSMakeRect(markerX, appKitY, markerSize, markerSize);
        markerPanel = [[NSPanel alloc]
            initWithContentRect:markerRect
                      styleMask:(NSWindowStyleMaskBorderless
                                 | NSWindowStyleMaskNonactivatingPanel)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [markerPanel setOpaque:YES];
        [markerPanel setHasShadow:NO];
        [markerPanel setHidesOnDeactivate:NO];
        [markerPanel setIgnoresMouseEvents:YES];
        [markerPanel setLevel:CGWindowLevelForKey(kCGFloatingWindowLevelKey)];
        [markerPanel setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces
                                             | NSWindowCollectionBehaviorStationary)];
        [markerPanel setBackgroundColor:[NSColor colorWithSRGBRed:1.0
                                                               green:0.0
                                                                blue:0.0
                                                               alpha:1.0]];
        [markerPanel orderFrontRegardless];
        [markerPanel displayIfNeeded];

        char durationArgument[16];
        char rectangleArgument[96];
        snprintf(durationArgument, sizeof(durationArgument), "-V%d", captureSeconds);
        snprintf(rectangleArgument,
                 sizeof(rectangleArgument),
                 "-R%d,%d,%d,%d",
                 rectX,
                 rectY,
                 rectWidth,
                 rectHeight);

        childProcesses = dispatch_group_create();
        dispatch_group_enter(childProcesses);
        dispatch_group_enter(childProcesses);

        char *captureArguments[] = {
            "/usr/sbin/screencapture",
            "-v",
            "-T0",
            durationArgument,
            rectangleArgument,
            "-x",
            (char *)argv[1],
            NULL,
        };
        spawnTracked(captureArguments[0], captureArguments);

        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                     (int64_t)markerDelayMs * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            [markerPanel setBackgroundColor:[NSColor colorWithSRGBRed:0.0
                                                                    green:1.0
                                                                     blue:0.0
                                                                    alpha:1.0]];
            [markerPanel displayIfNeeded];

            char *helperArguments[] = {
                (char *)argv[9],
                (char *)argv[10],
                (char *)argv[11],
                NULL,
            };
            spawnTracked(helperArguments[0], helperArguments);
        });

        dispatch_group_notify(childProcesses, dispatch_get_main_queue(), ^{
            [markerPanel orderOut:nil];
            [application stop:nil];
            NSEvent *wakeEvent = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                    location:NSZeroPoint
                                               modifierFlags:0
                                                   timestamp:0
                                                windowNumber:0
                                                     context:nil
                                                     subtype:0
                                                       data1:0
                                                       data2:0];
            [application postEvent:wakeEvent atStart:NO];
        });

        [application run];
        return exitStatus;
    }
}
