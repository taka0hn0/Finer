# Column hierarchy phase diagnosis — 2026-07-17

This focused run investigates the realistic-mixed Column View outliers from
the full 2026-07-17 Finder matrix. It used macOS 26.5.2 (25F84), Finder 26.4,
Karabiner-Elements 16.1.0, and an arm64 Mac15,12. Each variant ran ten fresh
Finder windows at 1,000 and 10,000 visible items and submitted `j l j` through
the installed Finer helper.

The environment records commit `7a16efc2497c84f5b7aeed0e92f2046c0549ac1c`
with `dirty=true` because the opt-in phase instrumentation was still an
uncommitted diagnostic change and an unrelated untracked strategy draft was
present. The retained baseline behavior is the behavior shipped with the phase
instrumentation. Its final repository build and installed helper both had
SHA-256 `3a07d26d5d9a6d2e14872bc0bae76fc8c45aca3e8e2e563ff989b69937b77e54`.

## Correctness

Both variants selected `01-A/item-00001.txt` in every iteration:

| Variant | 1,000 items | 10,000 items | Total |
| --- | ---: | ---: | ---: |
| Retained baseline | 10/10 | 10/10 | 20/20 |
| Temporary focus-only wait | 10/10 | 10/10 | 20/20 |

## Phase results

Values below are warm `l` p95 in milliseconds. With ten samples the nearest-rank
p95 is the maximum observed sample.

| Variant / items | Worker | Event post | Transition total | Old-item count probes | Focus probes | Candidate item array | Candidate selection |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Retained / 1,000 | 186.524 | 26.299 | 162.731 | 135.201 | 4.880 | 2.397 | 2.259 |
| Focus-only / 1,000 | 190.916 | 26.887 | 173.609 | 0.000 | 148.525 | 2.454 | 1.938 |
| Retained / 10,000 | 230.935 | 25.427 | 191.142 | 147.459 | 133.900 | 21.796 | 4.362 |
| Focus-only / 10,000 | 221.168 | 27.537 | 181.604 | 0.000 | 147.432 | 19.990 | 7.058 |

The temporary variant skipped the old container's item-count probe for ordinary
Column `AXList` containers and waited only for a different focused container.
This did not consistently reduce worker or transition p95. The long blocking
interval moved from the item-count phase to the focus phase. The evidence is
consistent with Finder delaying the first post-event AX query until the new
column becomes available, rather than Finer spending that interval scanning
items locally.

## Decision

- Keep the existing synchronization that prevents a queued `j` from operating
  on the previous column. The focus-only experiment was reverted.
- Keep the detailed phase counters behind
  `FINDER_VIM_COLUMN_PHASE_METRICS=1` together with the normal metrics flag.
  Normal use performs no phase clock sampling and writes no metrics.
- Do not describe the measured wait as file-content processing or local O(N)
  selection work. The blocking phase is Finder's asynchronous Column-to-AX
  publication boundary on this host.
- Investigate notification-based observation or true end-to-end frame timing
  separately. Do not replace the readiness check with a fixed delay or remove
  it merely to improve an internal latency number.

## Scope and files

These runs opened visible Finder windows and invoked the installed helper. They
exclude physical input, Karabiner evaluation, and Finder frame rendering, so
they are not end-to-end key latency measurements. Paths in the raw outcomes and
environment files are sanitized to `$REPO` and `$HOME`.

`baseline.*` contains the retained behavior. `focus-only.*` contains the
temporary experiment that was not adopted.
