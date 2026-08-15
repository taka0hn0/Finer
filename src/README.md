# Source layout

The three files directly under `src/` are stable executable entry points. The
implementation lives in responsibility-based subdirectories so build and
installation paths remain compatible.

## Navigation worker

`finder_ax_step.c` includes the fragments under `worker/finder_ax_step/` in an
explicit dependency order. They intentionally form one C translation unit to
preserve internal linkage and whole-file optimization on the navigation hot
path.

- `prelude.inc`: imports, shared types, metrics state, and forward declarations
- `accessibility.inc`: Finder process and Accessibility container discovery
- `selection.inc`: item identity, anchors, marks, and selection updates
- `events.inc`: locks, keyboard events, menus, and scroll primitives
- `movement.inc`: List, Column, and Icon movement algorithms
- `runtime.inc`: hold tokens, sockets, metrics, and runtime configuration
- `worker.inc`: command queue, worker lifecycle, and dispatch
- `hold_state.inc`: hold ownership and mode selection
- `hold_repeat.inc`: fast AX and native List repeat loops
- `edge_state.inc`: view-independent edge-monitor state machines
- `edge_monitor.inc`: Column/List edge monitoring and wrap execution
- `cli.inc`: command parsing and the executable entry point

Do not compile an `.inc` fragment independently. Add it to the ordered include
list in `finder_ax_step.c`; the Makefile already tracks every fragment as a
rebuild dependency.

## AX command helper

`commands/finder_ax_move/` separates common types, Accessibility operations,
navigation state, marks and selection, new-folder behavior, Visual selection,
and command dispatch. `finder_ax_move.swift` contains only the executable
entry point.

## Jump palette

`jump/` separates candidate search, Finder navigation, post-navigation zoxide
learning, AppKit UI, and headless test commands. `finer_jump.swift` contains
only the executable entry point.

Behavioral or architectural changes still belong in
`docs/FINDER_VIM_SPEC.md` and its Decision Log. Run `make check` after any
source change.
