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
