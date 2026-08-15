static bool direction_from_command(char command, direction_t *direction) {
    switch (command) {
        case 'j': *direction = direction_down; return true;
        case 'k': *direction = direction_up; return true;
        case 'h': *direction = direction_left; return true;
        case 'l': *direction = direction_right; return true;
        default: return false;
    }
}

static bool send_worker_command(
    char command_value,
    uint64_t submitted_nanoseconds
) {
    struct sockaddr_un address;
    if (!socket_address(&address)) return false;

    int descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (descriptor < 0) return false;

    worker_command_t command;
    memset(&command, 0, sizeof(command));
    command.submitted_nanoseconds = submitted_nanoseconds;
    command.command = command_value;
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

static bool run_toggle_mark_helper(void) {
    char helper_path[PATH_MAX];
    const char *override = getenv("FINDER_VIM_SWIFT_HELPER");
    if (override && override[0] != '\0') {
        if (snprintf(helper_path, sizeof(helper_path), "%s", override)
                >= (int)sizeof(helper_path)) {
            return false;
        }
    } else {
        const char *separator = strrchr(program_path, '/');
        if (!separator) return false;
        size_t directory_length = (size_t)(separator - program_path);
        const char helper_name[] = "/finder_ax_move";
        if (directory_length + sizeof(helper_name) > sizeof(helper_path)) {
            return false;
        }
        memcpy(helper_path, program_path, directory_length);
        memcpy(
            helper_path + directory_length,
            helper_name,
            sizeof(helper_name)
        );
    }

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

    char *arguments[] = {
        helper_path,
        "toggle-mark",
        NULL,
    };
    pid_t child;
    int spawn_error = posix_spawn(
        &child,
        helper_path,
        &file_actions,
        NULL,
        arguments,
        environ
    );
    posix_spawn_file_actions_destroy(&file_actions);
    if (spawn_error != 0) return false;

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool focused_navigation_container_changed(
    AXUIElementRef previous_container,
    column_phase_metrics_t *column_phases
) {
    uint64_t focus_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    AXUIElementRef application = NULL;
    if (!finder_is_frontmost(NULL, &application)) {
        if (column_phases) {
            column_phases->transition_focus_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                focus_started_nanoseconds
            );
        }
        return false;
    }

    CFTypeRef focused = copy_attribute(application, kAXFocusedUIElementAttribute);
    CFRelease(application);
    if (!focused || CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        if (column_phases) {
            column_phases->transition_focus_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                focus_started_nanoseconds
            );
        }
        return false;
    }

    navigation_role_t role;
    AXUIElementRef current = copy_nearest_navigation_container(
        (AXUIElementRef)focused,
        &role
    );
    CFRelease(focused);
    if (column_phases) {
        column_phases->transition_focus_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            focus_started_nanoseconds
        );
    }
    if (!current) return false;
    bool changed = !CFEqual(current, previous_container);
    if (changed) {
        uint64_t items_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        CFArrayRef items = copy_navigation_items(current, role);
        if (column_phases) {
            column_phases->transition_candidate_items_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                items_started_nanoseconds
            );
        }
        navigation_context_t candidate = {
            .container = current,
            .items = items,
            .role = role,
        };
        if (items && CFArrayGetCount(items) > 0) {
            uint64_t selection_started_nanoseconds = column_phases
                ? monotonic_nanoseconds()
                : 0;
            changed = current_index(&candidate) >= 0;
            if (column_phases) {
                column_phases
                    ->transition_candidate_selection_nanoseconds += counter_delta(
                        monotonic_nanoseconds(),
                        selection_started_nanoseconds
                    );
            }
        } else {
            changed = false;
        }
        if (items) CFRelease(items);
    }
    CFRelease(current);
    return changed;
}

static void wait_for_navigation_transition(
    const navigation_context_t *context,
    CFIndex previous_item_count,
    column_phase_metrics_t *column_phases
) {
    // CGEventPost is asynchronous. Serialize the following Vim command behind
    // Finder's view update so it cannot mutate the previous directory.
    uint64_t transition_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    bool transitioned = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (column_phases) ++column_phases->transition_attempts;
        uint64_t item_count_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        CFIndex item_count = raw_navigation_item_count(
            context->container,
            context->role
        );
        if (column_phases) {
            column_phases->transition_item_count_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                item_count_started_nanoseconds
            );
        }
        if (previous_item_count >= 0 && item_count != previous_item_count) {
            if (column_phases) {
                column_phases->transition_reason = column_transition_item_count;
            }
            transitioned = true;
            break;
        }
        if (focused_navigation_container_changed(
                context->container,
                column_phases
            )) {
            if (column_phases) {
                column_phases->transition_reason =
                    column_transition_focused_container;
            }
            transitioned = true;
            break;
        }
        uint64_t sleep_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        usleep(2000);
        if (column_phases) {
            column_phases->transition_sleep_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                sleep_started_nanoseconds
            );
        }
    }
    if (column_phases) {
        if (!transitioned) {
            column_phases->transition_reason = column_transition_timeout;
        }
        column_phases->transition_total_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            transition_started_nanoseconds
        );
    }
}

static CFIndex move_worker_context(
    navigation_context_t *context,
    bool *context_created,
    direction_t direction,
    CFIndex current,
    int movement_lock_fd,
    bool use_navigation_anchor,
    column_phase_metrics_t *column_phases
) {
    CFIndex previous_item_count = -1;
    if (direction == direction_left || direction == direction_right) {
        uint64_t item_count_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        previous_item_count = raw_navigation_item_count(
            context->container,
            context->role
        );
        if (column_phases) {
            column_phases->previous_item_count_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                item_count_started_nanoseconds
            );
        }
    }
    bool navigation_may_change = false;
    uint64_t movement_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    CFIndex position = move_once(
        context,
        direction,
        current,
        movement_lock_fd,
        &navigation_may_change,
        use_navigation_anchor,
        column_phases
    );
    if (column_phases) {
        column_phases->movement_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            movement_started_nanoseconds
        );
    }
    if (context->role == navigation_grid) {
        context->predicted_index = position > 0
                && (direction == direction_left || direction == direction_right)
            ? position - 1
            : -1;
    }
    if (navigation_may_change) {
        if (context->role != navigation_grid) {
            wait_for_navigation_transition(
                context,
                previous_item_count,
                column_phases
            );
        }
        navigation_context_release(context);
        *context_created = false;
    }
    return position;
}

static CFIndex worker_move(
    navigation_context_t *context,
    bool *context_created,
    direction_t direction,
    int movement_lock_fd,
    bool use_navigation_anchor,
    column_phase_metrics_t *column_phases
) {
    CFIndex validated_index = current_index_unknown;
    if (*context_created) {
        uint64_t validation_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        validated_index = current_index(context);
        if (column_phases) {
            column_phases->context_validation_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                validation_started_nanoseconds
            );
        }
    }
    if (*context_created && validated_index < 0) {
        navigation_context_release(context);
        *context_created = false;
    }
    if (!*context_created) {
        uint64_t creation_started_nanoseconds = column_phases
            ? monotonic_nanoseconds()
            : 0;
        *context_created = navigation_context_create(context);
        if (column_phases) {
            column_phases->context_creation_nanoseconds += counter_delta(
                monotonic_nanoseconds(),
                creation_started_nanoseconds
            );
        }
        if (!*context_created) return false;
        validated_index = current_index(context);
    }

    CFIndex position = move_worker_context(
        context,
        context_created,
        direction,
        validated_index,
        movement_lock_fd,
        use_navigation_anchor,
        column_phases
    );
    if (position > 0) return position;

    if (*context_created) navigation_context_release(context);
    uint64_t creation_started_nanoseconds = column_phases
        ? monotonic_nanoseconds()
        : 0;
    *context_created = navigation_context_create(context);
    if (column_phases) {
        column_phases->context_creation_nanoseconds += counter_delta(
            monotonic_nanoseconds(),
            creation_started_nanoseconds
        );
    }
    if (!*context_created) return 0;
    validated_index = current_index(context);
    return move_worker_context(
        context,
        context_created,
        direction,
        validated_index,
        movement_lock_fd,
        false,
        column_phases
    );
}

static CFIndex execute_worker_command(
    navigation_context_t *context,
    bool *context_created,
    char command,
    int movement_lock_fd,
    bool cold,
    uint64_t submitted_nanoseconds
) {
    if (command == 'e') {
        metrics_snapshot_t start;
        if (metrics_path) start = capture_metrics_snapshot();
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_EX);
        bool success = clear_focused_selection_fallback();
        if (!success && !*context_created) {
            *context_created = navigation_context_create(context);
        }
        if (!success && *context_created) {
            success = clear_selection(context);
        }
        if (*context_created) {
            navigation_context_release(context);
            *context_created = false;
        }
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_UN);
        if (metrics_path) {
            metrics_snapshot_t end = capture_metrics_snapshot();
            record_command_metrics(
                command,
                cold,
                submitted_nanoseconds,
                &start,
                &end,
                success ? 1 : 0,
                NULL
            );
        }
        return success ? 1 : 0;
    }

    if (command == 's') {
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_EX);
        if (*context_created) {
            navigation_context_release(context);
            *context_created = false;
        }
        bool success = run_toggle_mark_helper();
        if (movement_lock_fd >= 0) flock(movement_lock_fd, LOCK_UN);
        return success ? 1 : 0;
    }

    direction_t direction;
    if (!direction_from_command(command, &direction)) return 0;

    metrics_snapshot_t start;
    if (metrics_path) start = capture_metrics_snapshot();
    column_phase_metrics_t column_phases;
    column_phase_metrics_t *active_column_phases = NULL;
    if (column_phase_metrics_enabled) {
        memset(&column_phases, 0, sizeof(column_phases));
        active_column_phases = &column_phases;
    }
    CFIndex position = worker_move(
        context,
        context_created,
        direction,
        movement_lock_fd,
        true,
        active_column_phases
    );
    if (position > 0 && *context_created
        && context->has_marks
        && !write_navigation_anchor(context)) {
        position = 0;
    }
    if (metrics_path) {
        metrics_snapshot_t end = capture_metrics_snapshot();
        record_command_metrics(
            command,
            cold,
            submitted_nanoseconds,
            &start,
            &end,
            position,
            active_column_phases
        );
    }
    return position;
}

static int run_worker(
    char initial_command,
    int worker_lock_fd,
    uint64_t initial_submitted_nanoseconds
) {
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

    CFIndex initial_result = 0;
    if (AXIsProcessTrusted() && finder_is_frontmost(NULL, NULL)) {
        initial_result = execute_worker_command(
            &context,
            &context_created,
            initial_command,
            movement_lock_fd,
            true,
            initial_submitted_nanoseconds
        );
    }
    if (initial_result == 0) {
        if (movement_lock_fd >= 0) close(movement_lock_fd);
        if (context_created) navigation_context_release(&context);
        close(socket_fd);
        unlink(address.sun_path);
        if (worker_lock_fd >= 0) {
            flock(worker_lock_fd, LOCK_UN);
            close(worker_lock_fd);
        }
        flush_metrics();
        return 1;
    }

    struct pollfd poll_descriptor = {
        .fd = socket_fd,
        .events = POLLIN,
        .revents = 0,
    };
    while (poll(&poll_descriptor, 1, worker_idle_timeout_milliseconds) > 0) {
        if (!(poll_descriptor.revents & POLLIN)) break;

        worker_command_t command;
        ssize_t received = recv(socket_fd, &command, sizeof(command), 0);
        if (received != sizeof(command)) continue;
        if (!finder_is_frontmost(NULL, NULL)) break;
        CFIndex result = execute_worker_command(
            &context,
            &context_created,
            command.command,
            movement_lock_fd,
            false,
            command.submitted_nanoseconds
        );
        if (result == 0) {
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
    flush_metrics();
    return 0;
}

static bool enqueue_worker_command(char command, const char *command_name) {
    uint64_t submitted_nanoseconds = monotonic_nanoseconds();
    int worker_lock_fd = open_worker_lock();
    if (worker_lock_fd < 0) return false;

    bool lock_acquired = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (flock(worker_lock_fd, LOCK_EX | LOCK_NB) == 0) {
            lock_acquired = true;
            break;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            close(worker_lock_fd);
            return false;
        }
        if (send_worker_command(command, submitted_nanoseconds)) {
            close(worker_lock_fd);
            return true;
        }
        usleep(500);
    }
    if (!lock_acquired) {
        close(worker_lock_fd);
        return false;
    }

    // A live worker owns this lock for its full lifetime. If we acquired it,
    // any existing socket has no receiver and must not be treated as a
    // successful enqueue target.
    struct sockaddr_un address;
    if (socket_address(&address)) {
        unlink(address.sun_path);
    }

    char lock_descriptor[32];
    char submitted_time[32];
    snprintf(lock_descriptor, sizeof(lock_descriptor), "%d", worker_lock_fd);
    snprintf(
        submitted_time,
        sizeof(submitted_time),
        "%llu",
        (unsigned long long)submitted_nanoseconds
    );
    char *worker_arguments[] = {
        (char *)program_path,
        "worker",
        (char *)command_name,
        lock_descriptor,
        submitted_time,
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

static bool enqueue_worker_step(direction_t direction) {
    return enqueue_worker_command(
        direction_command(direction),
        direction_name(direction)
    );
}

static bool enqueue_toggle_mark(void) {
    return enqueue_worker_command('s', "toggle-mark");
}

static bool enqueue_clear_selection(void) {
    return enqueue_worker_command('e', "clear-selection");
}
