# Release checklist

Finer is pre-alpha. This checklist defines the evidence required before a
tagged development release. It does not imply that every item is already
complete.

## Repository and automated checks

- [ ] `main` is synchronized with the intended release commit.
- [ ] `make check` passes on the release commit.
- [ ] `make test-install` passes on the release commit.
- [ ] `make test-dist` reproduces and verifies the source archive from the
      release commit.
- [ ] GitHub Actions passes.
- [ ] `git diff --check` passes.
- [ ] No private paths, file names, clipboard data, logs, or runtime state are
      tracked.
- [ ] Generated Karabiner rules match their regression assertions.

## Installation lifecycle

- [ ] Fresh install succeeds.
- [ ] Reinstall succeeds.
- [ ] Existing modified importable rules are backed up.
- [ ] The main `karabiner.json` remains unchanged.
- [ ] Uninstall succeeds after active rules are disabled.
- [ ] Repeated uninstall is safe.
- [ ] Runtime state is preserved as documented.

## Manual Finder matrix

Record the macOS build, Karabiner version, keyboard layout, Finder UI language,
view settings, and tested commit.

- [ ] List View navigation, wrap, grouping, and disclosure behavior.
- [ ] Column View vertical and hierarchy navigation.
- [ ] Icon View horizontal and vertical navigation.
- [ ] Desktop navigation.
- [ ] Single taps, 100ms tap sequences, and held keys.
- [ ] Counts from 1 through representative two-digit values.
- [ ] Visual Mode `j/k`, counted `j/k`, `gg`, and `G` with a fixed anchor.
- [ ] Discontiguous marks remain visible while movement starts from the most
  recently marked item and updates only the transient current position.
- [ ] One `Esc` exits Normal or Visual Mode and deselects without changing
  Finder location.
- [ ] With confirmed marks, `y`, `x`, and `d` exclude the transient current
  position; without marks they use the current Finder selection.
- [ ] Copy, cut, paste, rename, and trash operations use only intended targets.
- [ ] Finder restart and window switching recover safely.

Test the supported combinations of:

- [ ] Japanese and English Finder UI.
- [ ] US and JIS keyboard layouts.
- [ ] The macOS versions claimed by the release notes.

## Performance and resources

- [ ] 10, 1,000, and 10,000-item fixtures are measured where required.
- [ ] List, Column, and Icon results are recorded.
- [ ] Empty-file and realistic-mixed profiles are distinguished.
- [ ] Held-key and 100ms tap-burst results contain no input drift.
- [ ] Cold, p50, p95, p99, and maximum values are reported where applicable.
- [ ] AX calls, process creation, CPU, wakeups, and memory are recorded for
      performance claims.
- [ ] No dedicated Finer process remains after the idle timeout.
- [ ] Helper-only measurements are labeled as internal and not presented as
      physical-key latency.
- [ ] Raw artifacts are sanitized before committing.

Follow [BENCHMARKS.md](BENCHMARKS.md) for the exact procedures.

## Documentation and release artifacts

- [ ] README status and known limitations are current.
- [ ] CHANGELOG contains the release changes.
- [ ] Troubleshooting covers known installation and permission failures.
- [ ] SECURITY lists the supported release series.
- [ ] Release notes identify breaking changes and migrations.
- [ ] Build artifacts include checksums.
- [ ] The source archive checksum matches a freshly rebuilt artifact from the
      same commit and version.
- [ ] The release clearly states supported macOS and Karabiner versions.
- [ ] The release clearly states whether it is alpha, beta, or stable.

## Final verification

- [ ] Install the exact release artifact, not a local development build.
- [ ] Repeat the critical navigation and file-operation smoke tests.
- [ ] Verify the tag and GitHub Release point to the same commit.
- [ ] Verify download links and checksums from a clean environment.
