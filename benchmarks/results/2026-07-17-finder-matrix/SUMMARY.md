# Finder view and input benchmark — 2026-07-17

This run measures Finer commit `246747e02e540f48f967d5f27cce1da9cd65cbe9`
on macOS 26.5.2 (25F84), Finder 26.4, Karabiner-Elements 16.1.0, and
an arm64 Mac15,12. The installed helper and the repository build both had
SHA-256 `a6adca91ebe8ebef3c4f005ba61f903d23e584cebdd4eb7a95efb7be12538497`.

The working tree reported `dirty=true` because the unrelated untracked file
`docs/PROJECT_STRATEGY.md` was present. Tracked source files were clean, and the
installed helper matched the build from the recorded commit.

## Scope and limitations

The view matrices opened visible Finder test windows and invoked the same
installed native helper used by the Karabiner rules. They do not include a
physical key event, Karabiner evaluation, or per-frame Finder rendering time.
The 100ms tap runner measures client scheduling and validates the final Finder
selection, but cannot prove that every intermediate frame was smooth.

Finder was set to name order. Grouping was recorded as `not-controlled` because
Finder can retain per-folder presentation state. These results are not grouped
List View coverage and must not be presented as full end-to-end key latency.

## Correctness

All 260 final-outcome iterations passed:

| Test family | Profiles | Iterations | Passed |
| --- | ---: | ---: | ---: |
| List, Column, and Icon view matrices | 2 | 180 | 180 |
| One-second held List navigation | 2 | 20 | 20 |
| 100ms tap scenarios | 2 | 60 | 60 |

The tap tests submitted 840 helper commands. Every client exited successfully,
every scenario ended on the expected Finder path, and neither held run changed
selection during the 100ms post-return observation window.

## View matrices

Each cell below is warm worker p95 in milliseconds. Column cells show `j / l`,
where `l` includes waiting for Finder to construct and expose the next column.

| Profile / view | 10 items | 1,000 items | 10,000 items | Passed |
| --- | ---: | ---: | ---: | ---: |
| empty-files / List `j` | 12.280 | 12.830 | 11.306 | 30/30 |
| realistic-mixed / List `j` | 12.114 | 17.275 | 13.595 | 30/30 |
| empty-files / Icon `l` | 0.480 | 0.307 | 0.291 | 30/30 |
| realistic-mixed / Icon `l` | 0.313 | 0.313 | 0.319 | 30/30 |
| empty-files / Column `j / l` | 53.090 / 215.441 | 48.378 / 186.705 | 56.403 / 210.605 | 30/30 |
| realistic-mixed / Column `j / l` | 41.676 / 202.353 | 55.900 / 230.465 | 58.081 / 482.061 | 30/30 |

The maximum measured physical footprint was 5,833,232 bytes, from the
10,000-item realistic Column run. No row reported a metrics drop or command
failure.

List worker time remained close across item counts and profiles. The
10,000-item List dispatch p95 was 128.078ms for empty files and 119.495ms for
realistic mixed content, while worker p95 was 11.306ms and 13.595ms. Most of
that observed dispatch interval is therefore outside the measured worker body.

Icon warm worker p95 remained at or below 0.480ms in all six cells. The
predicted-index path did not degrade at 10,000 items.

Column hierarchy movement remains the main performance concern. In the
realistic profile, the 1,000-item `j` and `l` dispatch p95 values were 713.074ms
and 660.663ms. At 10,000 items they were 651.974ms and 601.907ms, and the `l`
worker p95 reached 482.061ms. All final selections were correct, but these
outliers can produce visible pauses and need a focused follow-up measurement.

## One-second held navigation

Throughput is derived from the actual final Finder path. Stop time is measured
from clearing the hold token until the repeat helper returns.

| Profile | Passed | Total steps | Rate p50 | Rate p95 | Stop p50 | Stop p95 | Drift |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| empty-files | 10/10 | 780 | 75.136 steps/s | 79.769 steps/s | 20.910ms | 23.742ms | 0/10 |
| realistic-mixed | 10/10 | 766 | 73.594 steps/s | 77.345 steps/s | 20.299ms | 33.836ms | 0/10 |

The realistic profile was 2.1% lower at p50 than the empty-files profile. File
contents and logical byte size did not create a large held-navigation penalty
in this fixture.

## Rapid 100ms taps

Each profile ran ten iterations of List `j/k`, Icon `h/l`, and Column hierarchy
sequences against a 1,000-item fixture.

| Profile / scenario | Passed | Commands | Enqueue p95 | Submit lateness p95 | Client failures |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty-files / List | 10/10 | 130 | 13.349ms | 9.470ms | 0 |
| empty-files / Icon | 10/10 | 130 | 14.771ms | 9.436ms | 0 |
| empty-files / Column | 10/10 | 160 | 10.977ms | 9.326ms | 0 |
| realistic-mixed / List | 10/10 | 130 | 13.470ms | 9.489ms | 0 |
| realistic-mixed / Icon | 10/10 | 130 | 13.777ms | 9.658ms | 0 |
| realistic-mixed / Column | 10/10 | 160 | 10.443ms | 9.236ms | 0 |

These are scheduling values, not visual Finder latency. Final-path correctness
and the visible run showed no queued movement after completion, but physical
key and frame-timing validation remain separate release checks.

## Decision

- Keep the current List and Icon common paths for continued dogfood testing.
- Treat realistic Column hierarchy latency and its outliers as the next
  performance investigation.
- Keep file capacity and content profile separate in published results; item
  count and Finder metadata behavior matter more than logical bytes alone.
- Do not make a native-equivalent or end-to-end latency claim from this run.

## Files

The directory contains sanitized raw metrics, outcomes, tap samples, tap
summaries, and environment manifests. Local repository and home paths were
replaced with `$REPO` and `$HOME`. Synthetic fixture names were retained.
