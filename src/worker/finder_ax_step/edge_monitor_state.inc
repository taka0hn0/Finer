static void vertical_edge_monitor_state_create(
    vertical_edge_monitor_state_t *state
) {
    memset(state, 0, sizeof(*state));
    state->previous = -1;
}

static bool vertical_edge_monitor_observe(
    vertical_edge_monitor_state_t *state,
    CFIndex current,
    CFIndex boundary
) {
    if (current == boundary) {
        state->stable_observations = state->previous == current
            ? state->stable_observations + 1
            : 1;
        state->previous = current;
        return state->stable_observations >= 2;
    }

    state->stable_observations = 0;
    state->previous = current;
    return false;
}

static void vertical_edge_monitor_mark_wrapped(
    vertical_edge_monitor_state_t *state,
    CFIndex wrapped_target
) {
    state->stable_observations = 0;
    state->previous = wrapped_target;
}

static uint64_t vertical_edge_probe_delay_microseconds(
    CFIndex current,
    CFIndex boundary
) {
    const CFIndex near_edge_items = 12;
    const uint64_t fastest_step_microseconds = 8333;
    CFIndex distance = current > boundary
        ? current - boundary
        : boundary - current;
    if (distance <= near_edge_items) return fastest_step_microseconds;
    return (uint64_t)(distance - near_edge_items)
        * fastest_step_microseconds;
}

static int run_vertical_edge_monitor_state_self_test(void) {
    vertical_edge_monitor_state_t state;
    vertical_edge_monitor_state_create(&state);

    if (vertical_edge_monitor_observe(&state, 4, 9)
        || vertical_edge_monitor_observe(&state, 9, 9)
        || !vertical_edge_monitor_observe(&state, 9, 9)) {
        fprintf(stderr, "edge monitor self-test: stable boundary failed\n");
        return 1;
    }

    vertical_edge_monitor_mark_wrapped(&state, 0);
    if (vertical_edge_monitor_observe(&state, 0, 9)
        || state.stable_observations != 0
        || vertical_edge_monitor_observe(&state, 1, 9)
        || state.stable_observations != 0) {
        fprintf(stderr, "edge monitor self-test: rearm failed\n");
        return 1;
    }

    if (vertical_edge_monitor_observe(&state, 9, 9)
        || vertical_edge_monitor_observe(&state, 8, 9)
        || vertical_edge_monitor_observe(&state, 9, 9)
        || !vertical_edge_monitor_observe(&state, 9, 9)) {
        fprintf(stderr, "edge monitor self-test: reset failed\n");
        return 1;
    }

    vertical_edge_monitor_state_create(&state);
    if (vertical_edge_monitor_observe(&state, 3, 0)
        || vertical_edge_monitor_observe(&state, 0, 0)
        || !vertical_edge_monitor_observe(&state, 0, 0)) {
        fprintf(stderr, "edge monitor self-test: reverse boundary failed\n");
        return 1;
    }

    if (vertical_edge_probe_delay_microseconds(0, 0) != 8333
        || vertical_edge_probe_delay_microseconds(12, 0) != 8333
        || vertical_edge_probe_delay_microseconds(112, 0) != 833300
        || vertical_edge_probe_delay_microseconds(0, 112) != 833300) {
        fprintf(stderr, "edge monitor self-test: probe delay failed\n");
        return 1;
    }

    printf("Vertical edge monitor state tests passed.\n");
    return 0;
}
