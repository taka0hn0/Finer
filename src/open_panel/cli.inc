static bool open_panel_valid_command_argument(const char *argument) {
    return argument && argument[0] != '\0' && argument[1] == '\0'
        && strchr("hjklgGo", argument[0]) != NULL;
}

static int open_panel_self_test(void) {
    const char commands[] = "hjklGo";
    const CGKeyCode expected[] = {
        kVK_LeftArrow,
        kVK_DownArrow,
        kVK_UpArrow,
        kVK_RightArrow,
        kVK_DownArrow,
        kVK_Return,
    };
    for (size_t index = 0; index < sizeof(commands) - 1; ++index) {
        open_panel_key_event_t event;
        if (!open_panel_event_for_command(commands[index], false, &event)
            || event.key_code != expected[index]) {
            return 1;
        }
    }
    open_panel_key_event_t option_end;
    if (!open_panel_event_for_command('G', false, &option_end)
        || option_end.flags != kCGEventFlagMaskAlternate) {
        return 1;
    }
    for (const char *cursor = "hjklgGo"; *cursor; ++cursor) {
        open_panel_key_event_t original;
        if (!open_panel_event_for_command(*cursor, true, &original)) return 1;
    }
    if (open_panel_valid_command_argument("x")
        || open_panel_valid_command_argument("gg")
        || !open_panel_valid_command_argument("g")) {
        return 1;
    }
    if (!open_panel_context_model_accepts(
            "com.microsoft.VSCode",
            "open-panel",
            "AXList",
            "AXCollectionList"
        )
        || !open_panel_context_model_accepts(
            "com.microsoft.VSCode",
            "open-panel",
            "AXOutline",
            ""
        )
        || open_panel_context_model_accepts(
            "com.microsoft.VSCode",
            "save-panel",
            "AXList",
            ""
        )
        || open_panel_context_model_accepts(
            "com.microsoft.VSCode",
            "open-panel",
            "AXTextField",
            ""
        )
        || open_panel_context_model_accepts(
            "com.apple.finder",
            "open-panel",
            "AXOutline",
            ""
        )) {
        return 1;
    }

    // The production certification path reaches the allowlist above through
    // `open_panel_evaluation_accepts`, so cover that wrapper too: a focused
    // element with no AXSheet ancestor must never be certified.
    open_panel_evaluation_t evaluation;
    memset(&evaluation, 0, sizeof(evaluation));
    snprintf(
        evaluation.bundle_identifier,
        sizeof(evaluation.bundle_identifier),
        "%s",
        open_panel_bundle_id
    );
    snprintf(
        evaluation.sheet_identifier,
        sizeof(evaluation.sheet_identifier),
        "%s",
        open_panel_identifier
    );
    snprintf(evaluation.role, sizeof(evaluation.role), "AXList");
    evaluation.has_sheet = false;
    if (open_panel_evaluation_accepts(&evaluation)) return 1;
    evaluation.has_sheet = true;
    if (!open_panel_evaluation_accepts(&evaluation)) return 1;
    snprintf(
        evaluation.sheet_identifier,
        sizeof(evaluation.sheet_identifier),
        "save-panel"
    );
    if (open_panel_evaluation_accepts(&evaluation)) return 1;

    puts("Open panel helper self-test passed.");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        return open_panel_self_test();
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        return open_panel_set_variables(false, false) ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "diagnose") == 0) {
        return open_panel_print_diagnosis();
    }
    if (argc != 3 || strcmp(argv[1], "handle") != 0
        || !open_panel_valid_command_argument(argv[2])) {
        fprintf(stderr, "Usage: %s handle h|j|k|l|g|G|o\n", argv[0]);
        return 64;
    }

    signal(SIGINT, open_panel_signal_handler);
    signal(SIGTERM, open_panel_signal_handler);
    signal(SIGHUP, open_panel_signal_handler);
    atexit(open_panel_cleanup);
    return open_panel_run_session(argv[2][0]);
}
