// Exercise the real selection reader with a controlled AX response. No Finder
// window, Accessibility permission, or synthetic keyboard event is involved.
#include <ApplicationServices/ApplicationServices.h>
#include <assert.h>

static CFArrayRef published_selection;
static unsigned attribute_reads;
static unsigned attribute_writes;
static unsigned selection_writes;
static AXError selection_write_error = kAXErrorSuccess;
static bool apply_selection_write;
static AXError test_copy_attribute(
    AXUIElementRef element, CFStringRef name, CFTypeRef *value
) {
    (void)element;
    ++attribute_reads;
    if (CFEqual(name, kAXSelectedRowsAttribute)
        || CFEqual(name, kAXSelectedChildrenAttribute)) {
        if (!published_selection) return kAXErrorAttributeUnsupported;
        *value = CFRetain(published_selection);
        return kAXErrorSuccess;
    }
    // Simulate a stale per-item selected flag after deselection.
    if (CFEqual(name, kAXSelectedAttribute)) {
        *value = CFRetain(kCFBooleanTrue);
        return kAXErrorSuccess;
    }
    if (CFEqual(name, kAXURLAttribute)) {
        *value = CFRetain(CFSTR("file:///fixture/first"));
        return kAXErrorSuccess;
    }
    return kAXErrorAttributeUnsupported;
}

static AXError test_set_attribute(
    AXUIElementRef element, CFStringRef name, CFTypeRef value
) {
    (void)element;
    ++attribute_writes;
    if (CFEqual(name, kAXSelectedChildrenAttribute)) {
        ++selection_writes;
        if (apply_selection_write) {
            if (published_selection) CFRelease(published_selection);
            published_selection = CFArrayCreateCopy(NULL, (CFArrayRef)value);
        }
        return selection_write_error;
    }
    return kAXErrorSuccess;
}

#define AXUIElementCopyAttributeValue test_copy_attribute
#define AXUIElementSetAttributeValue test_set_attribute
#define main finer_cli_main
#include "../src/finder_ax_step.c"
#undef main
#undef AXUIElementCopyAttributeValue
#undef AXUIElementSetAttributeValue

int main(void) {
    // Completed synchronous steps use their own start time, not the old
    // timer phase. A slow step cannot accrue a backlog or an extra full slot.
    hold_pacer_t pacer = {.interval_ticks = 10, .next_step = 1000000};
    hold_pacer_complete_synchronous_step(&pacer, 100);
    assert(pacer.next_step == 110);
    hold_pacer_complete_synchronous_step(&pacer, 1000);
    assert(pacer.next_step == 1010);

    AXUIElementRef item = AXUIElementCreateApplication(100001);
    AXUIElementRef other = AXUIElementCreateApplication(100002);
    const void *items[] = {item};
    navigation_context_t context = {0};
    context.container = item;
    context.items = CFArrayCreate(NULL, items, 1, &kCFTypeArrayCallBacks);
    for (int role = navigation_outline; role <= navigation_grid; ++role) {
        context.role = (navigation_role_t)role;
        published_selection = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
        attribute_reads = 0;
        assert(current_index(&context) == -1);
        assert(attribute_reads == 1);
        CFRelease(published_selection);
        published_selection = CFArrayCreate(NULL, items, 1, &kCFTypeArrayCallBacks);
        assert(current_index(&context) == 0);
        CFRelease(published_selection);
    }
    context.role = navigation_outline;
    const void *unmapped[] = {other};
    published_selection = CFArrayCreate(NULL, unmapped, 1, &kCFTypeArrayCallBacks);
    attribute_reads = 0;
    assert(current_index(&context) == -1);
    assert(attribute_reads == 1);
    CFRelease(published_selection);
    published_selection = NULL;
    attribute_reads = 0;
    assert(current_index(&context) == 0);
    assert(attribute_reads == 2); // Unsupported array retains compatibility.

    // A persisted mark anchor must not turn an unselected first j
    // into a second-item selection. Confirm it is consumed but never applied.
    char anchor_path[] = "/tmp/finer-selection-test.XXXXXX";
    int fd = mkstemp(anchor_path);
    assert(fd >= 0);
    assert(dprintf(fd, "1\t0\tfile:///fixture/first\n") > 0);
    close(fd);
    assert(setenv("KARABINER_FINDER_ANCHOR_FILE", anchor_path, 1) == 0);
    context.role = navigation_list;
    context.cursor_index = -1;
    context.predicted_index = -1;
    published_selection = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
    attribute_writes = 0;
    assert(move_once_current_index(&context, -1, true) == -1);
    assert(attribute_writes == 0);
    assert(access(anchor_path, F_OK) != 0);
    assert(move_once_indexed(&context, direction_down, -1, 1) == 1);
    context.role = navigation_grid;
    assert(move_once_grid(&context, direction_down, -1, 1, NULL) == 1);

    // The measured-slower Column candidate must stay opt-in.
    assert(setenv("KARABINER_FINDER_MARKS_FILE", anchor_path, 1) == 0);
    unsetenv("FINDER_VIM_HOLD_COLUMN_NATIVE");
    context.role = navigation_list;
    assert(!supports_native_vertical_hold(&context, direction_down));
    assert(setenv("FINDER_VIM_HOLD_COLUMN_NATIVE", "1", 1) == 0);
    assert(supports_native_vertical_hold(&context, direction_down));
    assert(!supports_native_vertical_hold(&context, direction_right));
    context.role = navigation_grid;
    assert(!supports_native_vertical_hold(&context, direction_down));
    unsetenv("FINDER_VIM_HOLD_COLUMN_NATIVE");
    unsetenv("KARABINER_FINDER_MARKS_FILE");
    CFRelease(published_selection);
    published_selection = NULL;
    unsetenv("KARABINER_FINDER_ANCHOR_FILE");

    // Finder's grouped Column setter can mutate the selection and then
    // report unsupported. The first movement must succeed, not be retried.
    context.role = navigation_list;
    published_selection = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
    apply_selection_write = true;
    selection_write_error = kAXErrorAttributeUnsupported;
    selection_writes = 0;
    assert(move_once_indexed(&context, direction_down, -1, 1) == 1);
    assert(current_index(&context) == 0);
    assert(selection_writes == 1);
    apply_selection_write = false;
    CFRelease(published_selection);

    published_selection = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
    assert(!select_index(&context, 0)); // No selection is still a failure.
    CFRelease(published_selection);
    published_selection = CFArrayCreate(NULL, unmapped, 1, &kCFTypeArrayCallBacks);
    assert(!select_index(&context, 0)); // A different item is not success.
    CFRelease(published_selection);
    const void *multiple[] = {item, other};
    published_selection = CFArrayCreate(NULL, multiple, 2, &kCFTypeArrayCallBacks);
    assert(!select_index(&context, 0)); // Nor is a partial multi-selection.
    CFRelease(published_selection);
    published_selection = NULL;
    // Long holds use the same verified success-after-error behavior. They
    // must advance exactly once and keep wrapping on repeated cycles.
    CFRelease(context.items);
    const void *pair[] = {item, other};
    context.items = CFArrayCreate(NULL, pair, 2, &kCFTypeArrayCallBacks);
    CFMutableArrayRef scratch = CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
    fast_ax_scroll_state_t scroll = {0};
    apply_selection_write = true;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        selection_writes = 0;
        assert(select_fast_ax_next(&context, 0, direction_down, scratch, &scroll) == 1);
        assert(selection_writes == 1);
        assert(select_fast_ax_next(&context, 1, direction_down, scratch, &scroll) == 0);
        assert(select_fast_ax_next(&context, 0, direction_up, scratch, &scroll) == 1);
        assert(select_fast_ax_next(&context, 1, direction_up, scratch, &scroll) == 0);
    }
    apply_selection_write = false;
    selection_writes = 0;
    assert(select_fast_ax_next(&context, 0, direction_down, scratch, &scroll) == -1);
    assert(selection_writes == 1); // Genuine failures do not skip more files.
    CFRelease(scratch);
    CFRelease(published_selection);
    published_selection = NULL;

    CFRelease(context.items);
    CFRelease(item);
    CFRelease(other);
    puts("Navigation selection snapshot tests passed.");
    return 0;
}
