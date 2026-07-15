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

The worker is created on demand and exits after a short period without commands.
The current dogfood candidate is 750ms. This lets ordinary consecutive taps
reuse an AX context while still returning to zero dedicated resident processes
shortly after input stops.

Commands arriving after the worker lock is acquired but before its socket is
bound retry the socket briefly without blocking on the worker's full idle
lifetime. This keeps the first rapid input burst on one worker and prevents
startup-time command reordering.

The final default must be selected by comparing 300, 500, 750, 1000, and 1500ms.
Measure latency, peak/private memory duration, wakeups, and process creation; do
not choose solely from perceived responsiveness.

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
