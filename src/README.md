# Source layout

The four files directly under `src/` are stable executable entry points. The
implementation lives in responsibility-based subdirectories so build and
installation paths remain compatible.

`shared/` holds the Swift sources compiled into **both** Swift executables.
Add a file there only when the two helpers must behave identically:
`FinderScript.swift` owns the Apple Event scripts that retarget the front Finder
window, and `KeystrokeSocket.swift` owns the demo-overlay notification format.
The Makefile appends `SHARED_SWIFT_SOURCES` to `AX_MOVE_SOURCES` and
`JUMP_SOURCES`; keep both lists in sync when adding one.

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
  `column_edge_monitor.inc`, `list_edge_monitor.inc`,
  `edge_monitor_worker.inc`, and `edge_monitor_commands.inc`
- CLI: `cli_commands.inc` and `cli.inc`

`prelude.inc` owns every type that more than one fragment needs, so no fragment
has to trail a definition that only its successor uses just to satisfy the fixed
include order. It also owns the state file paths and the direction spellings
used by CLI arguments, worker datagrams, and token file names.
`accessibility_primitives.inc` owns the typed AX accessors, including
`copy_ax_element_attribute` and `perform_ax_action`; call those rather than
repeating a `CFGetTypeID` check or incrementing the metrics counters by hand.

The two delayed edge monitors are separate: `list_edge_monitor.inc` watches the
List selection settle on the boundary row, `column_edge_monitor.inc` owns the
Column tap-and-probe loop, and `edge_monitor_worker.inc` holds the lifetime and
cleanup shared by both.

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

`open_panel_context_model_accepts` in `context.inc` is the single allowlist
decision. Certification, the session's continued validation, and `diagnose` all
reach it through the same snapshot, and the self-test exercises that same
function, so the accepted applications and roles cannot drift away from what the
test covers.

Behavioral or architectural changes still belong in
`docs/FINDER_VIM_SPEC.md` and its Decision Log. Run `make check` after any
source change.
