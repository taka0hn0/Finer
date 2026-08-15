# Source layout

The four files directly under `src/` are stable executable entry points. The
implementation lives in responsibility-based subdirectories so build and
installation paths remain compatible.

## Navigation worker

`finder_ax_step.c` includes the fragments under `worker/finder_ax_step/` in an
explicit dependency order. They intentionally form one C translation unit to
preserve internal linkage and whole-file optimization on the navigation hot
path.

- Foundation: `prelude.inc`, `accessibility_primitives.inc`,
  `finder_accessibility.inc`, and `navigation_context.inc`
- Selection: `item_resolution.inc`, `selection_core.inc`,
  `selection_state.inc`, and `selection_clear.inc`
- Finder events: `process_locks.inc`, `keyboard_events.inc`, and
  `scroll_events.inc`
- Movement: `vertical_movement.inc`, `grid_movement.inc`, and
  `movement_dispatch.inc`
- Runtime and worker: `hold_tokens.inc`, `runtime_support.inc`, `metrics.inc`,
  `worker_transport.inc`, `navigation_transition.inc`,
  `worker_lifecycle.inc`, and `worker_client.inc`
- Hold navigation: `hold_state_tokens.inc`, `hold_configuration.inc`,
  `hold_position.inc`, `hold_scroll.inc`, `hold_fast_ax.inc`,
  `hold_native_list.inc`, and `hold_controller.inc`
- Edge handling: `edge_monitor_state.inc`, `edge_monitor_lock.inc`,
  `edge_probe.inc`, `column_stall_state.inc`, `column_edge_context.inc`,
  `column_edge_monitor.inc`, and `edge_monitor_commands.inc`
- CLI: `cli.inc`

Do not compile an `.inc` fragment independently. Add it to the ordered include
list in `finder_ax_step.c`; the Makefile already tracks every fragment as a
rebuild dependency.

## AX command helper

`commands/finder_ax_move/` separates common types, Accessibility operations,
navigation state, marks and selection, new-folder behavior, Visual selection,
and command dispatch. `finder_ax_move.swift` contains only the executable
entry point.

## Jump palette

`jump/` separates candidate models and ranking, zoxide and Spotlight clients,
Finder navigation, post-navigation learning, AppKit panel support, controller
lifecycle, layout, search coordination, result rendering, key input, selection
completion, and headless test commands. `finer_jump.swift` contains only the
executable entry point.

## Open panel navigation

`finer_open_panel.c` includes the fragments under `open_panel/` as one C
translation unit. `context.inc` owns the VS Code/open-panel AX allowlist,
`events.inc` owns native and fallback key events, `variables.inc` owns the two
Karabiner session variables, and `session.inc` owns the temporary socket,
AX-destruction observer, 250ms fallback validation, and cleanup. The helper
exists only while a verified Open panel is present.
`finer_open_panel diagnose` reports the current allowlist decision without
changing focus, variables, or key input.

Behavioral or architectural changes still belong in
`docs/FINDER_VIM_SPEC.md` and its Decision Log. Run `make check` after any
source change.
