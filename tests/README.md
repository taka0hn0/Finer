# Tests

The initial extraction relies on `make check` for build, JSON, shell syntax,
and personal-path checks.

Run `make test-install` for isolated packaging integration tests. It uses a
temporary `HOME` (including a space in the path), never writes to the dogfood
installation, and verifies repeat install/uninstall, backups, preflight failure,
file modes, preserved state, and an untouched main `karabiner.json`.

Planned suites are defined in `docs/FINDER_VIM_SPEC.md`:

- unit tests for state and movement calculations;
- integration tests for file operations in temporary directories;
- Finder AX fixtures for List, Column, and Icon views;
- reproducible latency and resource benchmarks.

## Manual race regression

Until the Finder AX integration harness exists, every navigation-cache change
must include this dogfood check:

1. Place a directory `A` above at least one sibling item.
2. Give `A` at least two children.
3. Focus the item immediately above `A` and type `jlj` as one fast burst.
4. Confirm that the second child inside `A` is focused.
5. Confirm that the sibling below `A` is never focused.

Repeat in Column View, with no deliberate delay and with a directory containing
many items.

## Automated Finder navigation baselines

After installing the current helper, run `make benchmark-list`,
`make benchmark-column`, and `make benchmark-icon`. Each runner creates and
closes a dedicated Finder window per iteration, verifies the final selected
path, and rejects incomplete or failed metrics.

The default empty-file fixtures isolate item-count and AX costs. Run
`make benchmark-realistic-views` to repeat the matrices with deterministic
mixed, non-empty local content; see `docs/BENCHMARKS.md` for the distinction.

Run `make benchmark-worker-timeout` after installing the current helper to
compare the five worker idle candidates across fixed two-tap gaps. The runner
requires the empty-files fixture and the appended worker-exit metrics field.

Run `make benchmark-hold` to record the current List View repeat throughput and
the upper-bound return time after the hold token is cleared. This benchmark is
the baseline for moving held repetition into the existing burst worker.

Run `make test-visual-latency-analyzer` for the headless 60fps synthetic-video
check of the red/green marker and PTS-based response detector. The visible
Column benchmark itself is `make benchmark-column-visual` or
`make benchmark-column-visual-realistic`; it records only a dedicated fixed
Finder rectangle and still excludes physical input and Karabiner evaluation.

Run `make test-finder-navigation` for functional regressions that do not belong
in the latency matrix. It covers grouped mixed-content List View wrap, a
one-second held List movement, and Icon View forward/reverse row wrap. The test
temporarily changes grouping only on its dedicated Finder window and restores
the original Finder grouping criterion and enabled state before closing it.
