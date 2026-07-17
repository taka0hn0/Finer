# Finer

[![CI](https://github.com/taka0hn0/Finer/actions/workflows/ci.yml/badge.svg)](https://github.com/taka0hn0/Finer/actions/workflows/ci.yml)

Fast, lightweight keyboard navigation and practical file operations for macOS
Finder.

> **Fast when active. Zero footprint when idle.**
>
> No dedicated background process, CPU usage, or periodic wakeups while idle.

The current development snapshot supports keyboard-first navigation, including
`h`/`j`/`k`/`l` motions, held-key movement, counts, Visual Mode, discontiguous
marks, and common Finder file operations.

## Status

The project is pre-alpha and has no supported release yet. Install the current
source snapshot only if you are comfortable testing early software that uses
macOS Accessibility and Karabiner-Elements.

## Design principles

- No dedicated resident process while idle.
- Reuse a small native worker during an active input burst.
- Measure latency, memory, CPU, wakeups, and process creation.
- Support Finder List, Column, and Icon views.
- Avoid dependence on the macOS UI language.
- Generate user-editable key mappings without replacing `karabiner.json`.

## Documentation

- [Troubleshooting and permissions](docs/TROUBLESHOOTING.md)
- [Product requirements and architecture](docs/FINDER_VIM_SPEC.md)
- [Performance architecture](docs/PERFORMANCE.md)
- [Benchmark procedure](docs/BENCHMARKS.md)
- [Release checklist](docs/RELEASE_CHECKLIST.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Changelog](CHANGELOG.md)

## Build

Requirements:

- macOS
- Xcode Command Line Tools
- Karabiner-Elements
- `jq` for the current development checks
- `ripgrep` for the current development checks

```sh
make build
make check
```

Build products are written to `.build/`.

## Benchmark fixtures

Generate the 10, 1000, and 10000-item Column View regression fixtures with:

```sh
make benchmark-fixtures
```

See [the benchmark procedure](docs/BENCHMARKS.md) before recording or
publishing performance results.

The latest complete Finder-window matrix is the
[2026-07-17 run](benchmarks/results/2026-07-17-finder-matrix/SUMMARY.md), which
covers both fixture profiles across List, Column, and Icon at 10, 1,000, and
10,000 items, plus held and 100ms tap scenarios.

After installing the current build, run the Column View matrix with:

```sh
make benchmark-column ITERATIONS=10
```

Run the direct-navigation matrices for List and Icon views with:

```sh
make benchmark-list ITERATIONS=10
make benchmark-icon ITERATIONS=10
```

Run the Finder functional navigation regressions with:

```sh
make test-finder-navigation
```

## Install the development snapshot

```sh
make install
```

This installs only generated artifacts:

- Helpers: `~/.local/libexec/finder-vim/`
- State: `~/.local/state/finder-vim/`
- Karabiner rule: `~/.config/karabiner/assets/complex_modifications/finder-vim.json`

Then open Karabiner-Elements Settings, choose **Complex Modifications**, add
the Finer rules, and enable them. The installer does not overwrite the
user's main `karabiner.json`.

When updating an existing installation, run `make install`, remove the enabled
copies of **Finer Navigation** and **Finer Utility Commands**, then add and
enable the newly installed copies. Karabiner keeps the previously imported
manipulators until the enabled rules are replaced.

To remove installed artifacts:

```sh
make uninstall
```

Disable the enabled rules in Karabiner-Elements before uninstalling.

Installation is safe to repeat. If an existing importable Finer rule differs
from the generated rule, it is backed up under
`~/.local/state/finder-vim/backups/` before replacement. Installation and
uninstallation preserve runtime state and never edit the main
`~/.config/karabiner/karabiner.json`.

## Build a source distribution

Create a deterministic source archive and adjacent SHA-256 file from the
current committed `HEAD` with:

```sh
make dist
make test-dist
```

Pass an explicit release identifier with `VERSION=0.1.0-alpha.1`. The archive
contains committed source only; tracked modifications must be committed before
packaging. `make test-dist` builds the same archive twice, checks byte-for-byte
reproducibility, verifies the checksum and safe archive paths, then runs
`make check` and the isolated install/uninstall suite from the extracted copy.
This pre-alpha artifact is built locally from source and is not a signed or
notarized binary package.

## Known limitations

- Only Finder is supported. Open/Save dialogs and Gallery View are outside the
  current scope.
- Supported macOS and Karabiner-Elements minimum versions have not been fixed.
- Installation still requires building from source and manually enabling two
  Karabiner complex-modification rules.
- List View `h`/`l` preserves Finder's native disclosure behavior. Some folders
  do not expose a disclosure triangle at a given location; use `o` to open the
  selected item instead.
- Published internal helper timings do not include the physical keyboard,
  Karabiner evaluation, or Finder frame rendering. They are not end-to-end
  latency claims.

See [Troubleshooting](docs/TROUBLESHOOTING.md) before filing a bug. Use the
repository Issue forms for reproducible bugs and feature proposals. Report
security-sensitive problems through the process in [SECURITY.md](SECURITY.md),
not in a public Issue.

## Repository policy

This repository is the source of truth. The live `~/.config/karabiner`
directory is an installation and dogfood environment, not the development
repository.
