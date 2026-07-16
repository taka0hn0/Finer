# Navigation A/B benchmark — 2026-07-16

This run compares the pinned legacy helper at `793a82c` (`baseline`) with the
optimized helper at `b55ab86` (`candidate`) on macOS 26.5.2 / Finder 26.4.
Both helpers were built with the same compiler and flags. The helper manifests,
sanitized raw samples, outcomes, summaries, and environments are stored beside
this file.

## Rapid 100ms taps

Each result is successful iterations out of ten. List runs ten `j` commands and
three `k` commands; Icon runs ten `l` commands and three `h` commands; Column
runs three `j l j h k` cycles followed by `j`.

| Helper / profile | List | Icon | Column |
| --- | ---: | ---: | ---: |
| baseline / empty-files | 10/10 | 0/10 | 10/10 |
| candidate / empty-files | 10/10 | 10/10 | 10/10 |
| baseline / realistic-mixed | 10/10 | 0/10 | 10/10 |
| candidate / realistic-mixed | 10/10 | 10/10 | 10/10 |

All client commands exited successfully. The baseline Icon sequence ended on
`item-00002` instead of `item-00007` in all 20 iterations. The candidate passed
all 60 final-path outcomes. Client enqueue p95 remained below 15ms in every
scenario, but this is not Finder render latency.

## One-second held navigation

Throughput is derived from the actual final Finder path relative to the known
fixture order. `Start p50` is the delay from the simulated key-down until the
repeat helper begins; `Stop p95` is the upper-bound delay from clearing the hold
token until the helper returns. Selection was sampled at return and 25, 50, and
100ms afterward.

| Helper / profile | Passed | Rate p50 | Start p50 | Stop p95 | Post-return drift |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline / empty-files | 10/10 | 10.459 steps/s | 1263.192ms | 37.006ms | 0/10 |
| candidate / empty-files | 10/10 | 74.098 steps/s | 279.073ms | 24.063ms | 0/10 |
| baseline / realistic-mixed | 1/10 | 0.000 steps/s | 1601.846ms | 216.707ms | 0/10 |
| candidate / realistic-mixed | 10/10 | 59.791 steps/s | 277.205ms | 33.667ms | 0/10 |

The candidate's reported internal AX position was exactly one greater than the
fixture file position in all 20 held iterations. This was stable from helper
return through 100ms and did not represent a queued movement. Finder can expose
a non-file AX row while the legacy helper and the fixture array count only file
items. Treating the internal AX position as the fixture index caused the first
benchmark pass to report false failures. The corrected runner retains the offset
for diagnosis but derives movement and correctness from Finder's actual path.

## Decision

The candidate is the forward implementation for continued dogfood testing:

- it fixes the deterministic Icon tap loss;
- it is about 7.1x faster than the baseline in the empty-files held test;
- it continues held movement in realistic mixed content where the baseline
  stopped in nine of ten iterations;
- it produced no post-return selection drift.

The realistic held p50 of 59.791 steps/s is slightly below the provisional
60 steps/s gate and should be confirmed by dogfood feel and another release
measurement. No public end-to-end latency claim should use this run because it
does not include physical input, Karabiner evaluation, or per-frame Finder
render timing. Grouping was not controlled.
