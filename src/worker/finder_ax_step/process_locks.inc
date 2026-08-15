static int open_movement_lock(void) {
    char path[PATH_MAX];
    state_path(path, sizeof(path), "finder_ax_step.lock");
    return open(path, O_CREAT | O_RDWR, 0600);
}

static int open_vertical_hold_controller_lock(void) {
    char path[PATH_MAX];
    state_path(path, sizeof(path), "finder_vertical_hold_controller.lock");
    return open(path, O_CREAT | O_RDWR, 0600);
}

static int open_worker_lock(void) {
    char path[PATH_MAX];
    state_path(path, sizeof(path), "finder_ax_step_worker.lock");
    return open(path, O_CREAT | O_RDWR, 0600);
}
