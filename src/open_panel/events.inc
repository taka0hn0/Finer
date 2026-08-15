typedef struct {
    CGKeyCode key_code;
    CGEventFlags flags;
} open_panel_key_event_t;

static bool open_panel_event_for_command(
    char command,
    bool original,
    open_panel_key_event_t *event
) {
    event->flags = 0;
    if (original) {
        switch (command) {
            case 'h': event->key_code = kVK_ANSI_H; return true;
            case 'j': event->key_code = kVK_ANSI_J; return true;
            case 'k': event->key_code = kVK_ANSI_K; return true;
            case 'l': event->key_code = kVK_ANSI_L; return true;
            case 'g': event->key_code = kVK_ANSI_G; return true;
            case 'G':
                event->key_code = kVK_ANSI_G;
                event->flags = kCGEventFlagMaskShift;
                return true;
            case 'o': event->key_code = kVK_ANSI_O; return true;
            default: return false;
        }
    }

    switch (command) {
        case 'h': event->key_code = kVK_LeftArrow; return true;
        case 'j': event->key_code = kVK_DownArrow; return true;
        case 'k': event->key_code = kVK_UpArrow; return true;
        case 'l': event->key_code = kVK_RightArrow; return true;
        case 'G':
            event->key_code = kVK_DownArrow;
            event->flags = kCGEventFlagMaskAlternate;
            return true;
        case 'o': event->key_code = kVK_Return; return true;
        default: return false;
    }
}

static bool open_panel_post_command(pid_t pid, char command, bool original) {
    open_panel_key_event_t mapping;
    if (!open_panel_event_for_command(command, original, &mapping)) return false;
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, mapping.key_code, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, mapping.key_code, false);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return false;
    }
    const CGEventFlags modifier_flags =
        kCGEventFlagMaskAlphaShift
        | kCGEventFlagMaskShift
        | kCGEventFlagMaskControl
        | kCGEventFlagMaskAlternate
        | kCGEventFlagMaskCommand
        | kCGEventFlagMaskSecondaryFn;
    CGEventFlags down_flags = CGEventGetFlags(down);
    CGEventFlags up_flags = CGEventGetFlags(up);
    CGEventSetFlags(
        down,
        (down_flags & ~modifier_flags) | mapping.flags
    );
    CGEventSetFlags(
        up,
        (up_flags & ~modifier_flags) | mapping.flags
    );
    if (original) {
        CGEventPostToPid(pid, down);
        CGEventPostToPid(pid, up);
    } else {
        pid_t frontmost_pid = 0;
        if (!open_panel_frontmost_pid(&frontmost_pid)
            || frontmost_pid != pid) {
            CFRelease(down);
            CFRelease(up);
            return false;
        }
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
    }
    CFRelease(down);
    CFRelease(up);
    return true;
}
