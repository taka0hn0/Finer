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

static bool post_key_event_with_flags(
    CGKeyCode key_code,
    bool key_down,
    bool autorepeat,
    CGEventFlags flags
) {
    CGEventRef event = CGEventCreateKeyboardEvent(NULL, key_code, key_down);
    if (!event) return false;
    // CGEventCreateKeyboardEvent can inherit the last synthetic modifier
    // state. Clear only modifier bits so a plain Arrow sent after an
    // Option+Arrow edge jump cannot become another edge jump, while retaining
    // Arrow-specific flags such as NumericPad and NonCoalesced.
    const CGEventFlags modifier_flags =
        kCGEventFlagMaskAlphaShift
        | kCGEventFlagMaskShift
        | kCGEventFlagMaskControl
        | kCGEventFlagMaskAlternate
        | kCGEventFlagMaskCommand
        | kCGEventFlagMaskSecondaryFn;
    CGEventFlags event_flags = CGEventGetFlags(event);
    event_flags &= ~modifier_flags;
    event_flags |= flags;
    CGEventSetFlags(event, event_flags);
    if (autorepeat) {
        CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, 1);
    }
    CGEventPost(kCGHIDEventTap, event);
    if (metrics_path) ++metrics_cg_events;
    CFRelease(event);
    return true;
}

static bool post_key_event(
    CGKeyCode key_code,
    bool key_down,
    bool autorepeat
) {
    return post_key_event_with_flags(key_code, key_down, autorepeat, 0);
}

static bool post_key_code(CGKeyCode key_code) {
    bool key_down_posted = post_key_event(key_code, true, false);
    bool key_up_posted = post_key_event(key_code, false, false);
    return key_down_posted && key_up_posted;
}

static bool post_key_code_with_flags(
    CGKeyCode key_code,
    CGEventFlags flags
) {
    bool key_down_posted = post_key_event_with_flags(
        key_code,
        true,
        false,
        flags
    );
    bool key_up_posted = post_key_event_with_flags(
        key_code,
        false,
        false,
        flags
    );
    return key_down_posted && key_up_posted;
}

static bool post_vertical_wrap_chord(
    pid_t finder_pid,
    direction_t direction
) {
    // Finder ignores the same synthetic chord when delivered with
    // CGEventPostToPid. Keep the native global event path, but close the
    // focus-switch race as far as possible by checking the captured Finder
    // process immediately before posting the down/up pair.
    if (!process_is_frontmost(finder_pid)) return false;
    CGKeyCode destination_edge_key = direction == direction_down
        ? kVK_UpArrow
        : kVK_DownArrow;
    return post_key_code_with_flags(
        destination_edge_key,
        kCGEventFlagMaskAlternate
    );
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

    AXUIElementRef menu_bar = copy_ax_element_attribute(
        application,
        kAXMenuBarAttribute
    );
    CFRelease(application);
    if (!menu_bar) return false;

    AXUIElementRef menu_item = copy_deselect_menu_item(menu_bar, 0);
    CFRelease(menu_bar);
    if (!menu_item) return false;
    AXError error = perform_ax_action(menu_item, kAXPressAction);
    CFRelease(menu_item);
    return error == kAXErrorSuccess;
}
