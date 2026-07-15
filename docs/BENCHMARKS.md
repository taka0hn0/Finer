# Benchmarks

Finder Vim performance claims must be based on reproducible Finder fixtures
and recorded environment metadata. This document defines the initial fixture
layout and result format; it does not claim measured latency yet.

## Prepare Column View race fixtures

```sh
make benchmark-fixtures
```

This creates ignored build data under:

```text
.build/benchmark-fixtures/column-race/
├── items-10/
├── items-1000/
└── items-10000/
```

Each case contains:

```text
00-start.txt
01-A/
├── item-00000.txt
├── item-00001.txt
└── ...
02-sibling.txt
```

The generator is idempotent for an unchanged fixture. It refuses to report
success when stale extra files make the item count inaccurate.

## Column View `jlj` regression

For each item count:

1. Open the case directory in a new Finder window.
2. Switch that window to Column View.
3. Focus `00-start.txt`.
4. Type `jlj` as one burst without deliberate delay.
5. Confirm the final selection is `01-A/item-00001.txt`.
6. Confirm `02-sibling.txt` is never selected after `l`.
7. Repeat at least ten times and record failures.

A new Finder window is required for each automated repetition. Setting Finder's
selection through AppleScript does not move AX focus back from a child column,
so reusing a navigated window can create an invalid test initial state.
The runner also raises that exact window ID and verifies the selected start path
after its readiness probe. Activating Finder without identifying the window is
insufficient when another Finder window already exists. It requires the start
selection to remain stable before measurement; Icon View uses a longer interval
because Finder can replace its selection while laying out a very large grid.

After `make install`, run the automated matrix with:

```sh
make benchmark-column ITERATIONS=10
```

Run a subset with `COUNTS=1000` or `COUNTS="10 1000"`. This is useful when a
GUI automation host limits the duration of one command; separate runs keep the
raw files independent and do not change the measurement procedure.

The runner creates a new Finder window per iteration, closes only that window,
and writes three ignored artifacts under `.build/benchmark-results/`:

- `*.metrics.tsv`: one worker record per command;
- `*.outcomes.tsv`: expected-path pass/fail results;
- `*.environment.txt`: commit and machine metadata.

Use a different helper or output directory with `FINDER_VIM_HELPER` and
`FINDER_VIM_RESULT_ROOT`.

Before each measured burst, the runner activates Finder and executes an
unmeasured synchronous `first` probe. This verifies that the parent Column View
AX context is ready and leaves `00-start.txt` selected. Therefore `cold` in the
metrics means a newly spawned Finder Vim worker; it does not mean an unwarmed
Finder process or untouched AX caches. Do not interact with other applications
while the matrix is running.

## Required environment metadata

Record these values with every raw result file:

- Finder Vim commit;
- macOS version and build;
- Finder version;
- Karabiner-Elements version;
- Mac model and processor;
- view mode, sort order, grouping, and preview-column state;
- cold or warm worker state;
- item count and content profile;
- iteration count.

## Worker metrics format

Set `FINDER_VIM_METRICS_FILE` to enable worker instrumentation. The automated
runner sets it for all three commands in each burst. Without this environment
variable the worker does not collect counters or write metrics.

```text
timestamp_ns
label
pid
worker_state
command
dispatch_to_selection_ns
worker_duration_ns
user_cpu_ns
system_cpu_ns
package_idle_wakeups
interrupt_wakeups
resident_bytes
physical_footprint_bytes
ax_reads
ax_writes
cg_events
result_position
dropped_records
```

`dispatch_to_selection_ns` starts when the helper client handles the command
and ends after the worker confirms Finder selection. It includes worker startup
and queued commands, but not the physical key event, Karabiner evaluation, or
shell-process launch. `worker_duration_ns` covers only the worker operation.

Records are buffered and written after the worker exits so file I/O does not
delay later commands in the same burst. The summary reports p50, p95, p99,
maximum, CPU, wakeups, AX calls, footprint, unique PIDs, failures, and dropped
records. Each automated runner waits for that flush before starting the next
iteration; this also guarantees that every iteration measures a new cold
worker rather than reusing the previous iteration's idle worker.

Preserve raw iterations before publishing aggregate values. Physical-key
latency, key-up-to-stop latency, and independent Instruments or `powermetrics`
validation remain separate measurements.

## End-to-end key timing limitation

Quartz `CGEventPost` is not a valid way to automate Finder Vim's full key path.
On the current test host, an injected `j` bypassed Karabiner-Elements and
reached Finder's native type-selection behavior. The locally installed
`karabiner_cli` exposes profile, variable, lint, and device-list operations but
does not expose key-event injection. Creating a pre-Karabiner virtual keyboard
with `IOHIDUserDevice` requires the
`com.apple.developer.hid.virtual.device` entitlement according to the installed
macOS SDK.

Do not publish a `CGEventPost`-to-AX value as Finder Vim key latency. A valid
end-to-end measurement requires either physical input with an independent
timestamp source or a suitably signed virtual-HID test tool. The internal
worker metrics remain useful, but their stated exclusion of the physical key,
Karabiner evaluation, and shell launch is mandatory.

## List and Icon direct navigation

The same files-only fixtures provide a first scalability comparison for direct
movement inside one directory:

```sh
make benchmark-list ITERATIONS=10
make benchmark-icon ITERATIONS=10
```

List View selects `item-00000.txt` and measures `jjj`; Icon View uses the same
selection and measures `lll`. Both must finish on `item-00003.txt`. Finder is
set to name order before each iteration. As with the Column runner, use
`COUNTS=1000` to run one size independently.

These runners measure the files-only, ungrouped-intent baseline. The current
AppleScript setup records grouping as `not-controlled`, because Finder can
retain per-folder presentation state that the script does not yet normalize.
Do not treat these results as grouped List View coverage.
