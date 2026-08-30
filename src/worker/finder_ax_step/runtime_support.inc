static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static uint64_t monotonic_nanoseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static bool socket_address(struct sockaddr_un *address) {
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;

    char path[PATH_MAX];
    state_path(path, sizeof(path), "finder_ax_step.sock");
    if (strlen(path) >= sizeof(address->sun_path)) return false;
    snprintf(address->sun_path, sizeof(address->sun_path), "%s", path);
    return true;
}

static uint64_t realtime_nanoseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}
