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
- Safe, repeatable development install and uninstall scripts with backup of a
  replaced importable rule.
- Reproducible 10, 1,000, and 10,000-item benchmark fixtures and sanitized
  benchmark results.
- macOS CI for builds, headless regressions, and isolated install/uninstall
  tests.

### Changed

- Product and public repository name standardized on Finer. Existing
  `finder-vim` paths remain compatibility identifiers.
- Worker idle timeout set to 750ms based on a reproducible comparison matrix.

### Fixed

- Rapid Column View hierarchy sequences no longer reuse a stale Finder
  container.
- Visual Mode counts and `gg`/`G` no longer leak characters into Finder's
  native name-selection behavior.
- Normal Mode `Esc` clears Finder selection without sending a competing global
  keyboard shortcut.
