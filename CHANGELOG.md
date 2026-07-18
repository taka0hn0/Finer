# Changelog

All notable changes to Finer will be documented in this file.

The project is currently pre-alpha and has not published a tagged release.

## [Unreleased]

### Added

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

- Rapid Column View hierarchy sequences no longer reuse a stale Finder
  container.
- Visual Mode counts and `gg`/`G` no longer leak characters into Finder's
  native name-selection behavior.
- Normal Mode `Esc` clears Finder selection without sending a competing global
  keyboard shortcut.
