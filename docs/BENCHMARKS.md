# Benchmarks

Finer performance claims must be based on reproducible Finder fixtures
and recorded environment metadata. This document defines the initial fixture
layout and result format; it does not claim measured latency yet.

## Fixture profiles

The `empty-files` profile isolates the cost of Finder item count and AX layout.
Its zero-byte files are intentional: it answers whether navigation scales with
10, 1000, or 10000 visible items without mixing in content decoding.

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

The separate `realistic-mixed` profile adds non-empty text, JSON, RTF, binary,
PPM and PNG files, directories, spaces, and Unicode names while preserving a
deterministic name order. It includes 64 KiB binary files and valid image files
that let Finder perform type, metadata, and thumbnail work:

```sh
make benchmark-realistic-fixtures
```

It is generated under `.build/benchmark-fixtures/realistic-mixed/`. Each
profile has a marker and `manifest.tsv` containing visible item, file,
directory, and logical-byte counts. A runner rejects a mismatched profile.

File capacity normally does not make Finer's AX step read the file contents.
It can still affect observed Finder responsiveness indirectly through metadata,
Quick Look, thumbnails, filesystem caching, external volumes, or File Provider
activity. Keep the empty and realistic results separate. The deterministic
local profile does not represent iCloud hydration or network storage; those
remain distinct manual test environments.

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

Run the same Column View regression against mixed non-empty content with:

```sh
make benchmark-column-realistic ITERATIONS=10
```

To diagnose Column hierarchy latency by phase without changing normal runtime
instrumentation, enable the additional opt-in counters:

```sh
FINDER_VIM_COLUMN_PHASE_METRICS=1 \
  make benchmark-column-realistic COUNTS="1000 10000" ITERATIONS=10
```

The Column runner then prints both the ordinary worker summary and
`summarize_column_phase_metrics.sh` output. The extra timing calls are made only
when both the normal metrics file and the Column phase flag are enabled.

Before each measured burst, the runner activates Finder and executes an
unmeasured synchronous `first` probe. This verifies that the parent Column View
AX context is ready and leaves `00-start.txt` selected. Therefore `cold` in the
metrics means a newly spawned Finer worker; it does not mean an unwarmed
Finder process or untouched AX caches. Do not interact with other applications
while the matrix is running.

## Required environment metadata

Record these values with every raw result file:

- Finer commit;
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
Metrics-dependent runners verify that the selected helper contains this
instrumentation before creating or activating a Finder window. A helper built
without metrics support fails immediately and can still be measured with the
separate held-navigation runner.

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
worker_exit_after_command_ns
column_phase_metrics_enabled
column_context_validation_ns
column_context_creation_ns
column_previous_item_count_ns
column_movement_ns
column_event_post_ns
column_transition_total_ns
column_transition_item_count_ns
column_transition_focus_ns
column_transition_candidate_items_ns
column_transition_candidate_selection_ns
column_transition_sleep_ns
column_transition_attempts
column_transition_reason
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

The appended Column fields keep the original first 19 columns stable.
`column_transition_reason` is `0` for no transition, `1` for an item-count
change, `2` for a ready focused container, and `3` for timeout. Rows collected
without the phase flag contain zeroes in these appended fields.

Preserve raw iterations before publishing aggregate values. Physical-key
latency, key-up-to-stop latency, and independent Instruments or `powermetrics`
validation remain separate measurements.

## Column screen-visible latency

Internal AX completion and visible Finder rendering have different boundaries.
After installing the current helper, run the fixed-window visual matrix with:

```sh
make benchmark-column-visual COUNTS="1000 10000" ITERATIONS=10
make benchmark-column-visual-realistic COUNTS="1000 10000" ITERATIONS=10
```

The visual runner requires `ffmpeg` and `ffprobe`, builds a benchmark-only
Objective-C capture helper, and creates a new fixed-size Column View window for
every repetition. It prepares `01-A`, waits for the normal burst worker to exit,
then records only that Finder rectangle. A small nonactivating marker inside the
rectangle changes from red to green immediately before the capture helper
spawns `finder_ax_step hold-start right`.

`analyze_visual_latency.py` uses each decoded frame's PTS. It keeps the last red
frame as the baseline, locates the first green frame, excludes the padded marker
rectangle, and reports the first later frame with a large controlled pixel
change. The runner separately requires the final path to be
`01-A/item-00000.txt` and records the normal worker metrics from the same
command. Videos, per-video JSON, samples, metrics, and environment metadata stay
under ignored `.build/benchmark-results/column-visual-*` directories. Review a
sample video before publishing results so an unrelated cursor or window change
cannot be mistaken for Finder's response.

Each sample row also includes the same command's worker state,
`dispatch_ms`, `worker_duration_ms`, and `visual_minus_dispatch_ms`. Interpret
the last field at the capture refresh resolution: a difference smaller than one
nominal frame does not establish that AX completion and visible presentation
occurred in a particular order.

Validate the PTS and change detector without opening Finder with:

```sh
make test-visual-latency-analyzer
```

That test generates a 60fps synthetic recording whose marker changes at 0.5s
and whose non-marker response changes at 0.7s, then requires a 200ms result.

The marker is displayed before `posix_spawn`, so the measured delta is a
conservative marker-to-visible value with approximately one frame of capture
and compositor quantization. It includes helper launch after the marker but
does not include the physical key, Karabiner evaluation, or the beginning of
its shell command. Keep the end-to-end limitation below attached to every
interpretation.

## End-to-end key timing limitation

Quartz `CGEventPost` is not a valid way to automate Finer's full key path.
On the current test host, an injected `j` bypassed Karabiner-Elements and
reached Finder's native type-selection behavior. The locally installed
`karabiner_cli` exposes profile, variable, lint, and device-list operations but
does not expose key-event injection. Creating a pre-Karabiner virtual keyboard
with `IOHIDUserDevice` requires the
`com.apple.developer.hid.virtual.device` entitlement according to the installed
macOS SDK.

Do not publish a `CGEventPost`-to-AX value as Finer key latency. A valid
end-to-end measurement requires either physical input with an independent
timestamp source or a suitably signed virtual-HID test tool. The internal
worker metrics remain useful, but their stated exclusion of the physical key,
Karabiner evaluation, and shell launch is mandatory.

## Worker idle timeout matrix

The public idle timeout is not selected by intuition. After installing the
current helper, compare the five candidates with two taps separated by fixed
submission gaps:

```sh
make benchmark-worker-timeout ITERATIONS=10
```

The default matrix tests 300, 500, 750, 1000, and 1500ms timeouts against 100,
400, 700, 1100, and 1600ms tap gaps in a ten-item List View. Override the
matrix with `FINDER_VIM_BENCHMARK_TIMEOUTS` or `FINDER_VIM_BENCHMARK_GAPS` for
diagnosis. Only the five specified timeout candidates are accepted.

`FINDER_VIM_BENCHMARK_IDLE_TIMEOUT_MS` is honored only while metrics are
enabled; it is a benchmark control, not a user configuration surface. The
runner records whether the pair reused one process, command latency and
resource counters, and the time from each process's final command to worker
flush. `worker_exit_after_command_ns` is appended after the existing metrics
columns so older summary tooling retains its field positions.

Choose the default using reuse at realistic tap gaps together with process
creation, exit delay, footprint, CPU, wakeups, and dogfood feel. A longer
timeout does not consume CPU while blocked in `poll`, but it retains the
worker's private memory for longer.

The decision run used ten repetitions for every candidate/gap pair. All 250
pairs passed with no dropped metrics or measured wakeups, and selected 750ms as
the default. See the [summary and sanitized raw data](../benchmarks/results/2026-07-16-worker-idle-timeout/SUMMARY.md).

## Held navigation throughput

Measure the existing one-second List View hold loop with:

```sh
make benchmark-hold ITERATIONS=10
```

The runner uses a 1000-item empty-files fixture, performs the initial `j`, waits
until the configured 150ms held threshold, then runs the current `hold-repeat`
path for one second. It records repeat steps per second and the upper-bound time
from clearing the hold token until the helper returns. Repeat throughput is
derived from the actual final Finder path, not the helper's internal AX row
position: Finder may expose non-file rows that do not have a one-to-one index in
the fixture. The runner records both positions and their offset for diagnosis.
It also records Finder selection immediately after return and after 25, 50, and
100ms. Any post-return selection drift is a failed iteration. Set
`FINDER_VIM_HOLD_DURATION_MS` or `FINDER_VIM_HELD_THRESHOLD_MS` to diagnose a
different interval.

Run the same hold path against the 1000-item realistic mixed fixture with:

```sh
make benchmark-hold-realistic ITERATIONS=10
```

Before leaving the machine to a visible Finder run, validate the helper, fixture
profile, item count, and deterministic name order without opening Finder:

```sh
make benchmark-hold-preflight
make benchmark-hold-realistic-preflight
```

These preflight targets never create result files or Finder windows.

This is an internal Finer-path measurement, not a physical-key comparison. It
includes the repeat helper's AX context creation and steady loop, but excludes
Karabiner's shell launch before the helper begins. A valid native comparison
still requires physical key input and an independent timestamp source, as
described in the end-to-end limitation above.

## Rapid `h/j/k/l` tap bursts

Long holds and repeated taps exercise different paths. A tap starts a new helper
client and sends one `hold-start` command to the reusable 750ms worker. Run the
100ms tap scenarios against the empty or realistic 1000-item fixture with:

```sh
make benchmark-taps ITERATIONS=10
make benchmark-taps-realistic ITERATIONS=10
```

The default matrix runs three fixed sequences:

- List View: ten `j` taps followed by three `k` taps;
- Icon View: ten `l` taps followed by three `h` taps;
- Column View: three `j l j h k` hierarchy cycles followed by `j`.

Each command is submitted against an absolute schedule, 100ms apart by default.
The runner records client enqueue duration, submission lateness, command status,
and final Finder selection. Override the interval with
`FINDER_VIM_TAP_INTERVAL_MS`; accepted values are 20–1000ms.

Validate both profiles without opening Finder or creating result files with:

```sh
make benchmark-taps-preflight
make benchmark-taps-realistic-preflight
```

`make check` also exercises the absolute scheduler at 50, 100, and 150ms with a
stub helper, verifies command-failure accounting and summary percentiles, and
confirms that preflight does not create a result directory. This headless test
does not launch or activate Finder.

Build a reproducible baseline/candidate pair without installing either helper:

```sh
make benchmark-comparison-helpers \
  BASELINE_REF=793a82c \
  CANDIDATE_REF=HEAD
```

The helpers are written to `.build/benchmark-helpers/baseline/` and
`.build/benchmark-helpers/candidate/`. Each directory includes an
`environment.txt` manifest with the requested ref, resolved commit or worktree
source, compiler, source SHA-256, and helper SHA-256. Pass either helper path as
`FINDER_VIM_HELPER` to a preflight or visible benchmark; building these files
does not change the installed dogfood helper.

The recorded 2026-07-16 baseline/candidate run, including sanitized raw data and
the correction for non-file AX rows in held-navigation position reporting, is
under
[`benchmarks/results/2026-07-16-navigation-ab/`](../benchmarks/results/2026-07-16-navigation-ab/SUMMARY.md).

This runner invokes the same helper command used by the generated Karabiner rule,
but it excludes physical input and Karabiner evaluation. Client enqueue duration
is not Finder selection latency, and final-path correctness cannot prove that
every intermediate frame was smooth. Observe the visible selection during the
run before making a product claim.

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

Run the same direct-navigation measurements against the realistic profile with:

```sh
make benchmark-list-realistic ITERATIONS=10
make benchmark-icon-realistic ITERATIONS=10
```

`make benchmark-realistic-views ITERATIONS=10` runs all three realistic
matrices. Use `COUNTS=10`, `COUNTS=1000`, or a quoted list to limit either
fixture generation or measurement.

## Recorded 2026-07-17 Finder matrix

The current full run covers empty-files and realistic-mixed profiles across
List, Column, and Icon at 10, 1,000, and 10,000 items. It also records
one-second held List movement and 100ms List, Icon, and Column tap scenarios.
All 260 final-outcome iterations passed.

The run confirmed stable List and Icon worker paths at 10,000 items, while
realistic Column hierarchy movement retained p95 outliers that require focused
follow-up. See the [summary and sanitized raw data](../benchmarks/results/2026-07-17-finder-matrix/SUMMARY.md).

This run opened visible Finder windows but still bypassed physical input and
Karabiner evaluation. It is not end-to-end key latency.
