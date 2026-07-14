# Extracted baseline

This repository began as an extraction from a working local
`~/.config/karabiner` installation on 2026-07-14.

## Included implementation

- `src/finder_ax_step.c`: transient C worker for navigation and held keys.
- `src/finder_ax_move.swift`: AX selection, marks, and clipboard information.
- `scripts/finder_action_marked.sh`: records copy, cut, and delete targets.
- `scripts/finder_paste.sh`: copies or moves recorded targets through Finder.
- `rules/generated/finder-vim.json`: snapshot of the two active Finder Vim
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

