# Extracted baseline

This repository began as an extraction from a working local
`~/.config/karabiner` installation on 2026-07-14.

## Included implementation

- `src/finder_ax_step.c` and `src/worker/finder_ax_step/`: transient C worker
  for navigation, held keys, worker transport, and edge monitoring.
- `src/finder_ax_move.swift` and `src/commands/finder_ax_move/`: AX selection,
  marks, new-folder behavior, Visual selection, and clipboard information.
- `src/finer_jump.swift` and `src/jump/`: on-demand folder and file search
  palette, candidate providers, and Finder navigation.
- `scripts/finder_action_marked.sh`: records copy, cut, and delete targets.
- `scripts/finder_paste.sh`: copies or moves recorded targets through Finder.
- `rules/generated/finder-vim.json`: snapshot of the two active Finer
  complex-modification rules, with personal absolute paths removed.

## Deliberately excluded

- Prebuilt binaries.
- Runtime state, sockets, locks, and logs.
- The user's complete `karabiner.json`.
- Older AppleScript and Swift navigation experiments not referenced by the
  extracted rules.
- The obsolete resident `finder_motion_helper` implementation.

The generated rule is a migration baseline, not the final configuration
system. A configurable rule generator remains a product requirement.
