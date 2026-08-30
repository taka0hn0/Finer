// Delayed edge monitor for List View. Finder owns the native Arrow repeat for
// the whole traversal; this loop only watches for the selection settling on the
// boundary row and then performs the standard edge move for the wrap.
//
// The Column view is watched by a different mechanism entirely
// (column_edge_monitor.inc), because a held Arrow there can block synchronous
// AX reads for over a second.
static int run_list_edge_monitor_loop(
    navigation_context_t *monitor_context,
    direction_t direction,
    int movement_lock_fd
) {
    CFIndex item_count = CFArrayGetCount(monitor_context->items);
    if (monitor_context->role != navigation_outline
        || item_count < 2
        || !mark_state_is_empty()) {
        EDGE_TRACE(
            "exit\tt=%.6f\treason=context-guard\trole=%d\texpected=%d\titems=%ld\n",
            monotonic_seconds(),
            monitor_context->role,
            navigation_outline,
            item_count
        );
        return 0;
    }

    CFIndex first = selectable_vertical_edge_index(monitor_context, false);
    CFIndex last = selectable_vertical_edge_index(monitor_context, true);
    CFIndex boundary = direction == direction_down ? last : first;
    CFIndex wrapped_target = direction == direction_down ? first : last;
    if (first < 0 || last < 0 || first == last) return 0;
    EDGE_TRACE(
        "context\tt=%.6f\titems=%ld\tfirst=%ld\tlast=%ld\tboundary=%ld\ttarget=%ld\n",
        monotonic_seconds(),
        item_count,
        first,
        last,
        boundary,
        wrapped_target
    );
    vertical_edge_monitor_state_t monitor_state;
    vertical_edge_monitor_state_create(&monitor_state);
    // A 1,000-item Column can take more than 30 seconds with Finder's actual
    // repeat cadence. Keep a safety cap, but make it long enough for a full
    // 10,000-item traversal while still ending immediately on key-up or focus
    // loss.
    double deadline = monotonic_seconds() + 300.0;

    while (monotonic_seconds() < deadline) {
        if (!arrow_key_is_down(direction)
            || !process_is_frontmost(monitor_context->finder_pid)) {
            EDGE_TRACE("exit\tt=%.6f\treason=release-or-focus\n", monotonic_seconds());
            break;
        }

        CFIndex current = current_index(monitor_context);
        if (current < 0) {
            usleep(50000);
            continue;
        }

        CFIndex distance = current > boundary
            ? current - boundary
            : boundary - current;
        bool viewport_available = false;
        bool viewport_near_boundary = false;
        if (current == boundary || distance > 12) {
            viewport_near_boundary = vertical_viewport_near_boundary(
                monitor_context,
                direction,
                &viewport_available
            );
        }
        bool boundary_mismatch = current == boundary
            && viewport_available
            && !viewport_near_boundary;
        CFIndex observed = boundary_mismatch
            ? direction == direction_down ? boundary - 1 : boundary + 1
            : current;

        bool should_wrap = vertical_edge_monitor_observe(
            &monitor_state,
            observed,
            boundary
        );
        EDGE_TRACE(
            "observe\tt=%.6f\tcurrent=%ld\tobserved=%ld\tboundary=%ld\tviewport_available=%d\tviewport_near=%d\tmismatch=%d\tstable=%u\twrap=%d\n",
            monotonic_seconds(),
            current,
            observed,
            boundary,
            viewport_available,
            viewport_near_boundary,
            boundary_mismatch,
            monitor_state.stable_observations,
            should_wrap
        );
        if (should_wrap) {
            bool arrow_held = arrow_key_is_down(direction);
            bool finder_frontmost = process_is_frontmost(monitor_context->finder_pid);
            bool marks_empty = mark_state_is_empty();
            EDGE_TRACE(
                "guard\tt=%.6f\tarrow=%d\tfrontmost=%d\tmarks_empty=%d\n",
                monotonic_seconds(),
                arrow_held,
                finder_frontmost,
                marks_empty
            );
            if (!arrow_held
                || !finder_frontmost
                || !marks_empty) {
                break;
            }

            CFIndex wrapped_position = 0;
            if (movement_lock_fd >= 0) {
                flock(movement_lock_fd, LOCK_EX);
            }
            CFIndex confirmed = current_index(monitor_context);
            if (confirmed == boundary
                && arrow_key_is_down(direction)
                && process_is_frontmost(monitor_context->finder_pid)) {
                wrapped_position = wrap_vertical_edge(
                    monitor_context,
                    direction
                );
            }
            if (movement_lock_fd >= 0) {
                flock(movement_lock_fd, LOCK_UN);
            }
            if (wrapped_position == 0) {
                EDGE_TRACE("exit\tt=%.6f\treason=wrap-failed\n", monotonic_seconds());
                return 1;
            }

            vertical_edge_monitor_mark_wrapped(
                &monitor_state,
                wrapped_target
            );
            EDGE_TRACE(
                "wrapped\tt=%.6f\tposition=%ld\tnext_delay_us=%llu\n",
                monotonic_seconds(),
                wrapped_position,
                (unsigned long long)vertical_edge_probe_delay_microseconds(
                    wrapped_target,
                    boundary
                )
            );
            // Do not read Finder selection immediately after the AX jump.
            // Let the still-held native Arrow own the entire next traversal,
            // and wait until even a conservative 120-steps/s stream could be
            // near the opposite boundary before asking Finder again.
            if (!wait_for_vertical_edge_delay(
                    monitor_context,
                    direction,
                    vertical_edge_probe_delay_microseconds(
                        wrapped_target,
                        boundary
                    ),
                    deadline
                )) {
                break;
            }
            continue;
        }

        // No AX readback can be useful before even a 120-steps/s repeat could
        // reach the near-edge window. Sleep in short chunks so key release and
        // Finder focus are still noticed promptly, then recalculate from one
        // fresh selection observation. This keeps ordinary native movement
        // free of periodic Finder AX contention.
        if (!wait_for_vertical_edge_probe(
                monitor_context,
                direction,
                current,
                boundary,
                viewport_near_boundary,
                boundary_mismatch,
                deadline
            )) {
            break;
        }
    }

    return 0;
}
