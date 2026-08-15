static bool open_panel_set_variables(bool active, bool g_prefix) {
    const char *cli = getenv("FINER_KARABINER_CLI");
    if (!cli || cli[0] == '\0') {
        cli = "/Library/Application Support/org.pqrs/Karabiner-Elements/bin/karabiner_cli";
    }
    char json[128];
    int length = snprintf(
        json,
        sizeof(json),
        "{\"finer_open_panel_active\":%d,\"finer_open_panel_g_prefix\":%d}",
        active ? 1 : 0,
        g_prefix ? 1 : 0
    );
    if (length <= 0 || (size_t)length >= sizeof(json)) return false;

    char *arguments[] = {
        (char *)cli,
        "--set-variables",
        json,
        NULL,
    };
    pid_t child = 0;
    if (posix_spawn(&child, cli, NULL, NULL, arguments, environ) != 0) {
        return false;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
