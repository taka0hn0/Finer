# Performance architecture

This document defines how Finer stays fast in directories containing many
items without introducing a permanent background process. Product-level
requirements remain in `FINDER_VIM_SPEC.md`.

## Performance model

Accessibility calls cross a process boundary into Finder. Their count matters
more than an equivalent amount of local array work. A navigation step should
therefore prefer:

1. one AX request that returns an array;
2. local Core Foundation array lookup;
3. a targeted AX write for the destination.

It should avoid asking every displayed file for attributes such as `AXSelected`
or recursively walking the complete view subtree.

## Scalable common path

The C burst worker follows these rules:

- Walk upward from the focused element and use the nearest supported Finder
  navigation container.
- Recursively inspect the focused window only as a compatibility fallback when
  there is no supported ancestor.
- Read `AXSelectedRows` or `AXSelectedChildren` once per selection lookup.
- In List View, retain the raw `AXRows` array without inspecting every row's
  child structure. Select only the target row through AX and advance to another
  candidate only if Finder rejects a nonselectable row.
- Resolve a selected descendant to its direct navigation child by following its
  short parent chain.
- Match AX elements against the cached item array inside the worker process.
- Keep per-item selection probing only as a compatibility fallback when Finder
  does not expose a selection array.
- After a horizontal command that can change directories, wait for Finder's
  container or item set to change and invalidate the cached AX context before
  processing the next command.
- When Column View creates a new container, require its item array to be
  available and its selected item to resolve inside that array before allowing
  the next queued command to rebuild its context.
- After posting a native Finder key event, yield briefly before polling AX so
  Finder can process the event instead of serving an immediate stream of stale
  selection queries.
- For Icon View horizontal bursts, retain the locally calculated destination as
  the next command's predicted index. Finder can apply queued arrow events while
  its AX selection still reports the pre-burst item.
- Recognize Finder's `AXCollectionList` Icon container separately from ordinary
  Column View `AXList` containers, and flatten its `AXSectionList` children once
  into the burst-local item array.

The target common-path cost is:

| Operation | AX work | Local work |
| --- | --- | --- |
| Container discovery | proportional to focused-element depth | constant |
| Selection lookup | one array read, plus a short parent chain if needed | at most O(N) array comparison |
| Selection update | one targeted write | constant |
| Warm repeated movement | independent of directory scan | cached-array lookup |

O(N) local comparison is acceptable as an intermediate implementation because
it does not perform N cross-process AX calls. It may later be replaced with a
cached index if measurements show a material benefit.

## Burst lifetime

The worker is created on demand and exits after 750ms without commands. This
lets ordinary consecutive taps reuse an AX context while returning to zero
dedicated resident processes shortly after input stops.

Commands arriving after the worker lock is acquired but before its socket is
bound retry the socket briefly without blocking on the worker's full idle
lifetime. This keeps the first rapid input burst on one worker and prevents
startup-time command reordering.

The default was selected by comparing 300, 500, 750, 1000, and 1500ms across
100, 400, 700, 1100, and 1600ms two-tap gaps, with ten repetitions per pair.
All 250 pairs passed without dropped metrics or measured wakeups.

| Timeout | Reused gaps | Processes / 50 pairs | Final idle residency | Max footprint |
| --- | --- | ---: | ---: | ---: |
| 300ms | 100ms | 90 | 27.116s | 2,589,104 bytes |
| 500ms | 100, 400ms | 80 | 40.114s | 2,605,488 bytes |
| 750ms | 100, 400, 700ms | 70 | 52.600s | 2,589,104 bytes |
| 1000ms | 100, 400, 700ms | 70 | 70.102s | 2,589,104 bytes |
| 1500ms | 100, 400, 700, 1100ms | 60 | 90.086s | 2,589,104 bytes |

750ms gives the same process count as 1000ms in this matrix with 25% less
post-command residency. It avoids the extra process creation seen at 500ms for
a 700ms gap, while 1500ms retains memory much longer for only ten fewer
processes. The recorded run is under
`benchmarks/results/2026-07-16-worker-idle-timeout/`.

For controlled measurement only, `FINDER_VIM_BENCHMARK_IDLE_TIMEOUT_MS`
selects one of those five candidates when worker metrics are enabled. Normal
operation ignores the variable and retains the compiled default. The timeout
matrix records process reuse across fixed tap gaps and the delay from a
process's final command until its metrics flush and exit path begins.

## List View row strategy

Finder can expose thousands of `AXRows` before it has populated every row's
cell descendants. Validating each row by walking those descendants made cold
movement proportional to the item count and could leave the cached array out
of sync with `AXSelectedRows` while Finder was still materializing rows.

The worker now keeps the raw row array and selects only the next target row.
When Finder rejects a nonselectable group row it advances to the next candidate.
It never clears selection by writing every row individually. Native arrow
movement was measured but rejected for this path: Finder's asynchronous
selection update caused queued commands to mistake an interior row for an edge.
Grouped List View, mixed file/folder directories, and long-held movement remain
required regressions for this strategy; no fallback may restore per-row AX
queries or writes to the common path.

## Required benchmark matrix

Test 10, 1000, and 10000 items in List, Column, and Icon views. Include files
only, folders only, mixed content, and grouped List View. For each case record:

- cold first-tap latency;
- warm p50, p95, p99, and maximum latency;
- key-up-to-stop latency;
- AX request count per command;
- process creations per input burst;
- CPU time, wakeups, RSS, and private memory;
- failures, skipped items, unintended jumps, and queued movement.

Raw results and the exact macOS, Finder, Karabiner, hardware, and view settings
must accompany any published performance claim.

The [2026-07-17 Finder matrix](../benchmarks/results/2026-07-17-finder-matrix/SUMMARY.md)
records both fixture profiles across List, Column, and Icon at 10, 1,000, and
10,000 items, plus held and 100ms tap scenarios. All final outcomes passed.
List and Icon worker timing remained stable at 10,000 items, but realistic
Column hierarchy movement showed substantial p95 outliers and remains the next
performance investigation. The run bypassed physical input and Karabiner
evaluation and is not an end-to-end latency measurement.

The focused [Column phase diagnosis](../benchmarks/results/2026-07-17-column-phases/SUMMARY.md)
then split realistic 1,000- and 10,000-item `j l j` runs into event posting,
transition probes, candidate-item acquisition, and context rebuilding. All 40
initial outcomes across the retained and focus-only variants passed. In the
retained path, warm `l` transition p95 was 162.731ms at 1,000 items and
191.142ms at 10,000 items. Removing the old-container item-count probe did not
consistently improve it: the wait moved to the focused-container AX probe,
whose p95 became 148.525ms and 147.432ms. This identifies Finder's asynchronous
Column-to-AX publication boundary, not local item scanning, as the dominant
phase on this host. The existing readiness synchronization remains in place to
protect rapid `jlj` correctness.

A follow-up AXObserver candidate waited for Finder-wide `AXCreated` or
`AXFocusedUIElementChanged` notifications before the same readiness check. In a
same-build comparison it reduced warm `l` AX reads from averages of 46.4 and
35.6 to 9.0, and measured wakeups from 9.1 and 6.4 to zero. However, worker p95
changed from 190.435ms to 192.365ms at 1,000 items and from 206.139ms to
212.130ms at 10,000 items. Dispatch p95 and active footprint also increased.
The candidate was therefore reverted: fewer active AX calls are useful only if
they do not trade away responsiveness or add unjustified lifecycle complexity.

## Benchmark instrumentation

Setting `FINDER_VIM_METRICS_FILE` enables per-command measurement inside the
burst worker. The client submission timestamp travels with the command so cold
startup and warm queued latency remain distinguishable. AX reads and writes,
CGEvent posts, process resource usage, wakeups, RSS, and physical footprint are
captured around the command.

Records remain in memory until worker exit. Normal operation without the
environment variable does not increment counters or write metric files.
Internal dispatch latency excludes the physical key-to-helper-launch path and
must not be presented as full keyboard latency.

`FINDER_VIM_COLUMN_PHASE_METRICS=1` adds detailed Column timing only when the
normal metrics file is also enabled. It remains a benchmark control rather than
a user-facing performance option.
