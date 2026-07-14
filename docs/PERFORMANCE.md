# Performance architecture

This document defines how Finder Vim stays fast in directories containing many
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
- Resolve a selected descendant to its direct navigation child by following its
  short parent chain.
- Match AX elements against the cached item array inside the worker process.
- Keep per-item selection probing only as a compatibility fallback when Finder
  does not expose a selection array.

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

The final default must be selected by comparing 300, 500, 750, 1000, and 1500ms.
Measure latency, peak/private memory duration, wakeups, and process creation; do
not choose solely from perceived responsiveness.

## Known remaining large-directory cost

Creating an outline context currently validates rows by inspecting their child
structure. That compatibility behavior can still issue AX calls proportional
to the row count on a cold start. Do not remove the validation until List View,
grouped List View, mixed file/folder directories, and long-held movement have
regression coverage.

Candidates to benchmark are:

- use the raw `AXRows` array and skip nonselectable rows only when a write fails;
- cache validated row identities only for the current burst;
- request row attributes in batches where macOS supports it;
- use native arrow movement for the interior path and direct AX selection only
  at boundaries.

No candidate may add an idle resident process.

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
