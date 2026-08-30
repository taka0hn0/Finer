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

// Spawns a detached child with its standard descriptors on /dev/null. The
// three helpers that start a process all need the same file actions, and
// failing to build them is reported instead of being silently ignored.
static bool spawn_with_null_stdio(
    const char *path,
    char *const arguments[],
    pid_t *child
) {
    posix_spawn_file_actions_t file_actions;
    if (posix_spawn_file_actions_init(&file_actions) != 0) return false;

    bool prepared =
        posix_spawn_file_actions_addopen(
            &file_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0
        ) == 0
        && posix_spawn_file_actions_addopen(
            &file_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0
        ) == 0
        && posix_spawn_file_actions_addopen(
            &file_actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0
        ) == 0;

    pid_t spawned = 0;
    bool spawned_ok = prepared
        && posix_spawn(
            &spawned,
            path,
            &file_actions,
            NULL,
            arguments,
            environ
        ) == 0;
    posix_spawn_file_actions_destroy(&file_actions);
    if (spawned_ok && child) *child = spawned;
    return spawned_ok;
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

    char *arguments[] = {
        helper_path,
        "toggle-mark",
        NULL,
    };
    pid_t child = 0;
    if (!spawn_with_null_stdio(helper_path, arguments, &child)) return false;

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
