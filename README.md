# Finder Vim

Fast, lightweight keyboard navigation and practical file operations for macOS
Finder.

> **Fast when active. Zero footprint when idle.**
>
> No dedicated background process, CPU usage, or periodic wakeups while idle.

This repository is an early development extraction of a working personal
Karabiner-Elements configuration. The current baseline supports keyboard-first
navigation, including familiar `h`/`j`/`k`/`l` motions, held-key movement,
counts, Visual Mode, discontiguous marks, and common Finder file operations.

## Status

The project is pre-alpha. See [the product requirements and architecture](docs/FINDER_VIM_SPEC.md)
and [the performance architecture](docs/PERFORMANCE.md)
before changing implementation or packaging decisions.

## Design principles

- No dedicated resident process while idle.
- Reuse a small native worker during an active input burst.
- Measure latency, memory, CPU, wakeups, and process creation.
- Support Finder List, Column, and Icon views.
- Avoid dependence on the macOS UI language.
- Generate user-editable key mappings without replacing `karabiner.json`.

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
the Finder Vim rules, and enable them. The installer does not overwrite the
user's main `karabiner.json`.

To remove installed artifacts:

```sh
make uninstall
```

Disable the enabled rules in Karabiner-Elements before uninstalling.

## Repository policy

This repository is the source of truth. The live `~/.config/karabiner`
directory is an installation and dogfood environment, not the development
repository.
