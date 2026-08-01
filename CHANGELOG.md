# Changelog

All notable changes to Finer will be documented in this file.

The project is currently pre-alpha and has not published a tagged release.

## [Unreleased]

### Added

- Finder-native folder and file icons, plus reliable `Command-A` text
  selection in the `z` palette.
- On-demand native Finder navigation with no dedicated resident process while
  idle.
- List, Column, and Icon View navigation, held-key movement, and numeric counts.
- Visual Mode range selection, including counted vertical movement and
  fixed-anchor `gg`/`G` motions.
- Discontiguous marks whose next movement starts from the most recently marked
  item.
- Persistent display of confirmed marks alongside a separate transient current
  position in List, Column, and Icon views.
- Safe, repeatable development install and uninstall scripts with backup of a
  replaced importable rule.
- Reproducible 10, 1,000, and 10,000-item benchmark fixtures and sanitized
  benchmark results.
- A complete Finder-window matrix for empty and realistic mixed content across
  List, Column, and Icon views, including held and 100ms tap scenarios.
- Opt-in Column hierarchy phase metrics and sanitized polling, focus-only, and
  AXObserver diagnostic results that separate Finder transition waiting from
  Finer context work.
- macOS CI for builds, headless regressions, and isolated install/uninstall
  tests.
- Reproducible commit-based source archives with adjacent SHA-256 files and
  extracted-artifact build/install verification.
- Separate source modules for the Navigation and Utility Commands Karabiner
  rules, with deterministic generation and CI drift detection.
- An on-demand `z` folder and file palette, combining zoxide folder history
  with Spotlight file results and same-window Finder selection.

### Changed

- Product and public repository name standardized on Finer. Existing
  `finder-vim` paths remain compatibility identifiers.
- Worker idle timeout set to 750ms based on a reproducible comparison matrix.
- Normal Mode `y`, `x`, and `d` target only confirmed marks when any exist;
  the transient current position remains excluded.
- Visual Mode `Esc` now clears the mode and Finder selection in one press.
- Unmarked vertical holds in List View now use Finder-native arrow auto-repeat
  with limited boundary probing and wrap handling. Column View uses a
  predicted-index AX loop on an absolute 8.333ms timeline. Both paths stop from
  the release token without changing taps, marked navigation, or idle residency.

### Fixed

- The `z` palette uses a wider, single-line plain text field sized to its
  standard proportional system font, so descenders remain visible without
  excess space below the text.
- The `z` palette ignores AppKit's transient last-window termination requests
  during input-source changes and closes only from explicit completion or an
  original physically marked `Esc`.
- Characters sent immediately after a Command-key Japanese input-source switch
  now stay in the `z` palette instead of leaking to Finder during focus recovery.
- Pressing `Return` during Japanese composition now confirms the marked text
  instead of prematurely accepting a jump candidate and closing the palette.
- Keys typed into the `z` palette are no longer intercepted by Finder-facing
  Finer mappings, so multi-character and Japanese input stay in the field.
- The `z` palette no longer reactivates its field editor from every input-source
  notification, avoiding an IME focus loop and beachballing while idle.
- Rapid Column View hierarchy sequences no longer reuse a stale Finder
  container.
- Visual Mode counts and `gg`/`G` no longer leak characters into Finder's
  native name-selection behavior.
- Normal Mode `Esc` clears Finder selection without sending a competing global
  keyboard shortcut.
