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
- For a normal vertical hold in List or Column View, read the starting
  selection once, advance a burst-local predicted index with the same modulo
  rule at every directory size, and write only each destination. Verify the
  real selection when the hold stops instead of after every repeat.
- Map `Shift+j/k` Boost Mode directly to Finder-native arrow auto-repeat. It
  does not start a worker, inspect AX state, or wrap at an edge.

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

## Precise hold and Boost Mode

Normal `j/k` in List and Column View uses the precise AX path derived from the
2026-07-15 implementation. The loop keeps the navigation item array and current
index locally, calculates the next destination with modulo arithmetic, and
schedules repeats on an absolute 8.333ms timeline with `mach_wait_until`. When
an AX write misses a tick, it skips the expired tick instead of issuing a
catch-up write. It reuses one mutable single-item selection array and verifies
the real selection when the hold stops. This path wraps without a separate edge
observer and checks the release token immediately before each write.

`Shift+j/k` is Boost Mode. Karabiner maps it directly to Finder's repeating
Arrow and starts no helper process. Finder owns selection rendering, viewport
tracking, repeat speed, and stop-at-edge behavior. Boost is disabled in text
input, Visual Mode, while confirmed marks may exist, and while a motion count
is active.

The following native Column observer remains default-off diagnostic history,
not the product path selected by normal `j/k` or Boost Mode.

A default-off `finder_native_column_hold_experiment` maps uncounted, unmarked
Normal Mode `j/k` directly to Finder's repeating arrow events. Karabiner limits
the path to a focused `AXList` without the `AXCollectionList` subrole, which
distinguishes Column from Icon View on the measured host. Marked movement,
Visual Mode, and counted motions continue to use the verified worker path. The
default path keeps Finder's native stop-at-edge behavior. A second default-off
`finder_native_column_edge_wrap_experiment` launches a transient edge worker
after 100ms. The worker start creates its release token immediately before
spawning, so no shell process precedes the direct Arrow on initial key-down.
Once Karabiner's delayed action has passed its pressed-variable condition, the
Column observer treats the token generation and captured frontmost Finder PID
as its lifetime authority. It does not sample the synthetic Arrow through
`CGEventSourceKeyState`, because that combined-session state can disappear
while the physical `j/k` hold is still active. Physical key-up truncates the
token, and a later same-direction hold writes a new generation that invalidates
the predecessor. The new worker retries the same-direction monitor lock every
2ms for at most 250ms so the invalidated process can release it. List keeps its
existing Arrow-state lifetime check.
After validating the token, frontmost Finder, and empty mark set, the Column
worker releases the continuously held logical Arrow before creating its AX
context. It then posts complete Arrow down/up taps every 16.667ms. This avoids
asking Finder for synchronous AX state while an Arrow remains held, which took
about 1.5 seconds in the 50ms-polling dogfood check even though ordinary native
movement stayed fast. The worker does not build the generic navigation snapshot
or copy the full `AXChildren` array. It retains only the nearest Column `AXList`,
the captured Finder PID, and the previously selected child. Every 50ms it reads
the one selected child. A changed child means Finder is still traversing; the
same child means movement has stalled at the requested edge. The same rule is
used for every directory size and in both directions. It does not read the
item count, `AXIndex`, edge children, remaining distance, or scrollbar value.

On a stall, the worker posts Finder's native `Option+Up Arrow` or
`Option+Down Arrow` document-edge command and continues the same 16.667ms
complete-Arrow tap loop. Immediately before every
document-edge command it checks that the Finder PID captured at startup is
still frontmost. Finder did
not process an otherwise successful `CGEventPostToPid` Option+Arrow trial as a
document-edge command, so the working frontmost-event path remains in use;
global event monitors can therefore still observe this synthetic chord. A
per-direction token communicates physical key release. Column edge wrapping
and physical-input throughput remain adoption tests rather than established
performance claims.

The change targets the delay before that native chord. A diagnostic build
observed generic Column context creation taking about 1.53s while Finder's
native repeat was active. Once the old observer had confirmed the boundary, two
first-wrap chord samples completed in about 36.1ms and 45.3ms; 24 later
predicted wraps had a p95 chord-posting time of about 0.325ms. These small
diagnostic samples identify the remaining initialization bottleneck but do not
yet establish the physical end-to-end p95 target.

The refresh-rate-independent integration test opens dedicated 1,000-item List
and Column windows and invokes the monitor's actual edge action at both
boundaries. It verifies the resulting Finder selection path: List uses the AX
wrap-and-scroll action, while Column uses Finder's native Option+Arrow document
edge action. List boundary discovery probes only a short edge prefix for
selectable file rows so Finder group headings do not become wrap targets and
startup work does not grow with directory size.

The List monitor retains its distance-based probe schedule and avoids periodic
AX reads while the selection cannot be near a boundary. Column deliberately
uses the simpler fixed 50ms selected-child comparison instead. Earlier
physical-key A/B checks found that broader 50ms polling across a 1,000-item
Column could make native traversal feel slower. The current candidate retests
that period with a lightweight context that reads only the selected child and
keeps 100ms as the rollback baseline. Its ordinary-path throughput and wrap
latency remain dogfood acceptance checks rather than assumed improvements.

A trace of the direct-only candidate showed a successful AX wrap about 2.88s
after monitor start and a selected first item throughout the remainder of the
hold, but Finder displayed that jump only after key-up. Thus a held logical
Arrow and an external AX jump cannot be treated as independent owners. The
current candidate retains direct Karabiner movement until the first edge, then
uses the established worker event loop for restart and later wraps. It also
accepts the first boundary observation when Column selection and scroll edge
agree, avoiding the extra stable sample. The worker lifetime is 300 seconds so
long 1,000- and 10,000-item traversals can be exercised.

In a subsequent 60Hz physical-key dogfood check with 1,000 items, the user
reported that Column `j/k` hold speed felt comparable to the adopted List
path. This is qualitative evidence that bypassing per-step AX selection removes
the observed bottleneck; it is not a measured throughput or 120Hz rendering
result.

The optimization does not change taps, counted motions, Icon movement, Visual
Mode, or marked navigation. Marked movement retains the verified path because
Finder's visible selection must continue to contain the confirmed marks plus
the transient cursor.

In the retained three-iteration 1,000-item internal measurements, the earlier
direct-AX candidate raised List throughput from 58.236 to 95.756 steps/s at p50
and Column from 23.820 to 32.510 steps/s. All retained baseline and candidate
iterations had the expected final path and zero drift through 100ms after
return. Those measurements did not include physical input or screen tracking;
the direct-AX List candidate could therefore select rows outside the visible
viewport despite its stronger internal throughput. See the
[sanitized comparison](../benchmarks/results/2026-07-17-hold-fast-path/SUMMARY.md).

A subsequent physical-key dogfood check used the same 1,000-item List fixture.
From `item-00500`, a manual approximately one-second `j` hold ended on
`item-00576` while the visible range moved to `541–579` and the vertical scroll
value moved from `0.500` to `0.563`. The matching `k` hold ended on
`item-00418`, with visible range `413–452` and scroll value `0.432`. An upward
wrap from `item-00005` ended on `item-00944` with scroll value `0.975`; a
downward wrap from `item-00994` ended on `item-00027` with scroll value `0`.
These are functional screen-tracking observations from Finder's Accessibility
tree, not instrumented throughput samples.

## List View row strategy

Finder can expose thousands of `AXRows` before it has populated every row's
cell descendants. Validating each row by walking those descendants made cold
movement proportional to the item count and could leave the cached array out
of sync with `AXSelectedRows` while Finder was still materializing rows.

The worker now keeps the raw row array and selects only the next target row.
When Finder rejects a nonselectable group row it advances to the next candidate.
It never clears selection by writing every row individually. Native arrow
movement remains unsuitable for independent taps and counted movement because
Finder's asynchronous selection update can cause queued commands to mistake an
interior row for an edge. The held List path is separate: it avoids per-event
selection inference and probes only after the theoretical boundary distance.
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

## Screen-visible timing

Worker timing ends when Finer has confirmed Finder's AX selection. It does not
say exactly when the selection highlight became visible. The separate Column
visual benchmark records a fixed rectangle containing only its dedicated
Finder window and a small nonactivating color marker. The marker changes from
red to green immediately before the existing helper is spawned. Analysis uses
the recording's presentation timestamps (PTS), not `frame_number / 60`, because
macOS screen recordings may omit unchanged frames.

The reported value starts on the first green frame and ends on the first frame
whose non-marker pixels exceed the controlled change threshold. The selected
path and the worker metrics from the same `right` command must also pass. The
marker is updated before helper launch, so the result is a conservative
marker-to-visible bound with approximately one capture-frame quantization. It
includes helper process launch and Finder rendering after the marker, but it
still excludes physical input and Karabiner evaluation. Each visual sample also
records the same command's dispatch and worker durations plus
`visual_minus_dispatch_ms`; this separates a visible-rendering gap from time
already accounted for by Finer's internal completion boundary.

The marker, capture process, and analyzer are benchmark-only tools. They are
never installed and do not change Finer's zero-residency runtime architecture.

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
