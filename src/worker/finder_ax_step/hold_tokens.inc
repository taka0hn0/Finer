static void token_path(char *buffer, size_t size, direction_t direction) {
    const char *name;
    switch (direction) {
        case direction_down: name = "finder_down_hold.txt"; break;
        case direction_up: name = "finder_up_hold.txt"; break;
        case direction_left: name = "finder_left_hold.txt"; break;
        case direction_right: name = "finder_right_hold.txt"; break;
    }
    state_path(buffer, size, name);
}

static bool write_hold_token(direction_t direction) {
    char path[PATH_MAX];
    token_path(path, sizeof(path), direction);
    int descriptor = open(path, O_CREAT | O_RDWR, 0600);
    if (descriptor < 0) return false;
    if (flock(descriptor, LOCK_EX) != 0) {
        close(descriptor);
        return false;
    }

    uint64_t random_values[2];
    arc4random_buf(random_values, sizeof(random_values));
    char token[64];
    int length = snprintf(
        token,
        sizeof(token),
        "%" PRIu64 "\t%016" PRIx64 "%016" PRIx64,
        monotonic_nanoseconds(),
        random_values[0],
        random_values[1]
    );
    bool success = length > 0
        && length < (int)sizeof(token)
        && ftruncate(descriptor, 0) == 0
        && pwrite(descriptor, token, (size_t)length, 0) == length;
    flock(descriptor, LOCK_UN);
    close(descriptor);
    return success;
}

static bool clear_hold_token(direction_t direction) {
    char path[PATH_MAX];
    token_path(path, sizeof(path), direction);
    int descriptor = open(path, O_CREAT | O_RDWR, 0600);
    if (descriptor < 0) return false;
    if (flock(descriptor, LOCK_EX) != 0) {
        close(descriptor);
        return false;
    }
    bool success = ftruncate(descriptor, 0) == 0;
    flock(descriptor, LOCK_UN);
    close(descriptor);
    return success;
}

static ssize_t read_token(int descriptor, char *buffer, size_t size) {
    ssize_t count = pread(descriptor, buffer, size - 1, 0);
    if (count < 0) return count;
    buffer[count] = '\0';
    return count;
}

static bool token_created_within(
    int descriptor,
    uint64_t maximum_age_nanoseconds
) {
    char token[64];
    ssize_t token_length = read_token(descriptor, token, sizeof(token));
    if (token_length <= 0) return false;

    char *separator = strchr(token, '\t');
    if (!separator) return false;
    *separator = '\0';
    char *end = NULL;
    errno = 0;
    uint64_t created_nanoseconds = strtoull(token, &end, 10);
    uint64_t now = monotonic_nanoseconds();
    return errno == 0
        && end
        && *end == '\0'
        && now >= created_nanoseconds
        && now - created_nanoseconds < maximum_age_nanoseconds;
}

static bool hold_token_created_within(
    direction_t direction,
    uint64_t maximum_age_nanoseconds
) {
    char path[PATH_MAX];
    token_path(path, sizeof(path), direction);
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return false;
    if (flock(descriptor, LOCK_SH) != 0) {
        close(descriptor);
        return false;
    }
    bool recent = token_created_within(
        descriptor,
        maximum_age_nanoseconds
    );
    flock(descriptor, LOCK_UN);
    close(descriptor);
    return recent;
}

static bool stop_hold_token(direction_t direction) {
    const uint64_t new_generation_guard_nanoseconds = 100000000ULL;
    char path[PATH_MAX];
    token_path(path, sizeof(path), direction);
    int descriptor = open(path, O_CREAT | O_RDWR, 0600);
    if (descriptor < 0) return false;
    if (flock(descriptor, LOCK_EX) != 0) {
        close(descriptor);
        return false;
    }

    bool preserve_new_generation = token_created_within(
        descriptor,
        new_generation_guard_nanoseconds
    );

    bool success = preserve_new_generation
        || ftruncate(descriptor, 0) == 0;
    flock(descriptor, LOCK_UN);
    close(descriptor);
    return success;
}

static int run_hold_token_stop_self_test(void) {
    char path[PATH_MAX];
    token_path(path, sizeof(path), direction_down);

    if (!write_hold_token(direction_down)) return 1;
    if (!stop_hold_token(direction_down)) return 1;
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return 1;
    char token[64];
    ssize_t token_length = read_token(descriptor, token, sizeof(token));
    close(descriptor);
    if (token_length <= 0) return 1;

    if (!write_hold_token(direction_up)) return 1;
    if (!hold_token_created_within(direction_up, 100000000ULL)) return 1;

    usleep(110000);
    if (hold_token_created_within(direction_up, 100000000ULL)) return 1;
    if (!stop_hold_token(direction_down)) return 1;
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return 1;
    token_length = read_token(descriptor, token, sizeof(token));
    close(descriptor);
    if (token_length != 0) return 1;

    if (!write_hold_token(direction_up)
        || !clear_hold_token(direction_up)) {
        return 1;
    }
    token_path(path, sizeof(path), direction_up);
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return 1;
    token_length = read_token(descriptor, token, sizeof(token));
    close(descriptor);
    return token_length == 0 ? 0 : 1;
}
