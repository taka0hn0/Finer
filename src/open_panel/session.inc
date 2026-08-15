static void open_panel_signal_handler(int signal_number) {
    (void)signal_number;
    open_panel_should_stop = 1;
}

static void open_panel_observer_callback(
    AXObserverRef observer,
    AXUIElementRef element,
    CFStringRef notification,
    void *reference
) {
    (void)observer;
    (void)element;
    (void)reference;
    if (CFStringCompare(
        notification,
        kAXUIElementDestroyedNotification,
        0
    ) == kCFCompareEqualTo) {
        open_panel_destroyed = 1;
    }
}

static void open_panel_cleanup(void) {
    if (open_panel_variables_active) {
        (void)open_panel_set_variables(false, false);
        open_panel_variables_active = false;
    }
    if (open_panel_socket_fd >= 0) {
        close(open_panel_socket_fd);
        open_panel_socket_fd = -1;
    }
    if (open_panel_socket_path[0] != '\0') {
        unlink(open_panel_socket_path);
        open_panel_socket_path[0] = '\0';
    }
    if (open_panel_lock_fd >= 0) {
        close(open_panel_lock_fd);
        open_panel_lock_fd = -1;
    }
}

static bool open_panel_create_state_paths(
    char *lock_path,
    size_t lock_path_size
) {
    char state_directory[PATH_MAX];
    int directory_length = snprintf(
        state_directory,
        sizeof(state_directory),
        "%s/.local/state/finder-vim",
        open_panel_home_directory()
    );
    if (directory_length <= 0
        || (size_t)directory_length >= sizeof(state_directory)) {
        return false;
    }
    if (mkdir(state_directory, 0700) != 0 && errno != EEXIST) return false;
    if (snprintf(
        open_panel_socket_path,
        sizeof(open_panel_socket_path),
        "%s/finer_open_panel.sock",
        state_directory
    ) >= (int)sizeof(open_panel_socket_path)) {
        return false;
    }
    int lock_length = snprintf(
        lock_path,
        lock_path_size,
        "%s/finer_open_panel.lock",
        state_directory
    );
    return lock_length > 0 && (size_t)lock_length < lock_path_size;
}

static bool open_panel_send_existing_command(char command) {
    int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_fd < 0) return false;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(open_panel_socket_path) >= sizeof(address.sun_path)) {
        close(socket_fd);
        return false;
    }
    strcpy(address.sun_path, open_panel_socket_path);
    bool sent = sendto(
        socket_fd,
        &command,
        1,
        0,
        (const struct sockaddr *)&address,
        sizeof(address)
    ) == 1;
    close(socket_fd);
    return sent;
}

static bool open_panel_bind_session_socket(void) {
    open_panel_socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (open_panel_socket_fd < 0) return false;
    int flags = fcntl(open_panel_socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(open_panel_socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, open_panel_socket_path);
    unlink(open_panel_socket_path);
    return bind(
        open_panel_socket_fd,
        (const struct sockaddr *)&address,
        sizeof(address)
    ) == 0;
}

static void open_panel_process_session_command(pid_t pid, char command) {
    if (command == 'g') {
        (void)open_panel_set_variables(true, true);
        return;
    }
    (void)open_panel_set_variables(true, false);
    (void)open_panel_post_command(pid, command, false);
}

static int open_panel_run_session(char first_command) {
    char lock_path[PATH_MAX];
    if (!open_panel_create_state_paths(lock_path, sizeof(lock_path))) return 1;
    open_panel_lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (open_panel_lock_fd < 0) return 1;

    if (flock(open_panel_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (open_panel_send_existing_command(first_command)) return 0;
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 2000000};
            nanosleep(&delay, NULL);
        }
        if (flock(open_panel_lock_fd, LOCK_EX | LOCK_NB) != 0) {
            pid_t pid = 0;
            if (open_panel_frontmost_pid(&pid)) {
                (void)open_panel_post_command(pid, first_command, true);
                return 0;
            }
            return 1;
        }
    }
    if (!open_panel_bind_session_socket()) return 1;

    open_panel_context_t context;
    if (!open_panel_copy_context(true, &context)) {
        pid_t pid = 0;
        if (open_panel_frontmost_pid(&pid)) {
            (void)open_panel_post_command(pid, first_command, true);
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 2000000};
            nanosleep(&delay, NULL);
            char queued[32];
            ssize_t received;
            while ((received = recv(
                open_panel_socket_fd,
                queued,
                sizeof(queued),
                0
            )) > 0) {
                for (ssize_t index = 0; index < received; ++index) {
                    if (open_panel_frontmost_pid(&pid)) {
                        (void)open_panel_post_command(pid, queued[index], true);
                    }
                }
            }
        }
        return 0;
    }

    if (!open_panel_set_variables(true, first_command == 'g')) {
        (void)open_panel_post_command(context.pid, first_command, true);
        open_panel_release_context(&context);
        return 1;
    }
    open_panel_variables_active = true;
    if (first_command != 'g') {
        (void)open_panel_post_command(context.pid, first_command, false);
    }

    AXObserverRef observer = NULL;
    CFRunLoopSourceRef observer_source = NULL;
    if (AXObserverCreate(
        context.pid,
        open_panel_observer_callback,
        &observer
    ) == kAXErrorSuccess) {
        if (AXObserverAddNotification(
            observer,
            context.sheet,
            kAXUIElementDestroyedNotification,
            NULL
        ) == kAXErrorSuccess) {
            observer_source = AXObserverGetRunLoopSource(observer);
            CFRunLoopAddSource(
                CFRunLoopGetCurrent(),
                observer_source,
                kCFRunLoopDefaultMode
            );
        }
    }

    uint64_t started = open_panel_monotonic_milliseconds();
    uint64_t next_fallback = started + (uint64_t)open_panel_fallback_interval_ms;
    while (!open_panel_should_stop && !open_panel_destroyed) {
        char commands[32];
        ssize_t received;
        while ((received = recv(
            open_panel_socket_fd,
            commands,
            sizeof(commands),
            0
        )) > 0) {
            for (ssize_t index = 0; index < received; ++index) {
                open_panel_process_session_command(context.pid, commands[index]);
            }
        }

        (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, true);
        uint64_t now = open_panel_monotonic_milliseconds();
        if (now - started >= (uint64_t)open_panel_max_session_seconds * 1000ULL) {
            break;
        }
        if (now >= next_fallback) {
            if (!open_panel_session_context_is_valid(context.pid)) break;
            next_fallback = now + (uint64_t)open_panel_fallback_interval_ms;
        }
    }

    if (observer_source) {
        CFRunLoopRemoveSource(
            CFRunLoopGetCurrent(),
            observer_source,
            kCFRunLoopDefaultMode
        );
    }
    if (observer) CFRelease(observer);
    open_panel_release_context(&context);
    return 0;
}
