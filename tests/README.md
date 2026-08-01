# Tests

The initial extraction relies on `make check` for build, JSON, shell syntax,
and personal-path checks.

`make check-rules` reconstructs the tracked importable Karabiner JSON from the
separate Navigation and Utility Commands source modules, requires byte-for-byte
snapshot equality, rejects invalid module descriptions and symlinked output,
and verifies deterministic regeneration in an isolated directory.

`make check` also runs the headless vertical-edge-monitor state tests. They
verify two-observation edge confirmation, reset after leaving an edge, reset
after a completed wrap, and distinct List/Column direction locks. They also
verify the List distance-based AX probe delay in both directions. The separate
Column state test verifies that a changed selection continues traversal and an
identical consecutive selection triggers the item-count-independent stall
path.
The suite exercises pure state and lock self-test entry points rather than a
live worker, so it does not open Finder or require a refresh-rate-specific
display.
The lock self-test also forks a predecessor and successor for the same Column
direction, releases the predecessor after 20ms, and requires the successor's
bounded 2ms retry loop to acquire the lock. This covers repeated physical
holds without relying on a live Finder window.

The headless `finer_jump` suite injects fake zoxide and Spotlight executables
and verifies folder frecency, folder/file basename priority, path matches,
multi-token filtering, directory exclusion from file results, combined
ranking, and clean source-unavailable errors. It also injects a fake
`finder_ax_move` and requires folder navigation and file reveal to delegate
the exact path through `jump-to` and `reveal-file`. It does not open the
palette or Finder.

Input-source changes remain a physical dogfood check. While the `z` palette is
open, repeated English/Japanese switching must not repeatedly call
`makeFirstResponder`, beachball, close the palette, or break text editing.
Generated-rule tests verify that every Finer manipulator is disabled while the
palette owns `finer_jump_active`, and that the launch wrapper resets the
variable when the palette exits. They also require the physical-Esc marker to
accept only an original, unchanged Karabiner event. Dogfood still checks that
`n`, `hjkl`,
multi-character text, `Delete`, and Japanese composition reach the search
field, then work as Finer commands again after closing it.
During Japanese composition, the first `Return` must confirm marked text while
keeping the palette open; only a subsequent `Return` may accept a jump target.
Arrow keys and `Esc` must remain available to the IME while marked text exists.
The physical input-source check must also cover pressing the left Command key
alone and immediately typing `n`, `j`, and longer romaji sequences. Those
characters must reach the palette before its 50ms fallback focus check; they
must never reach Finder.
Leave the palette untouched for at least five seconds before and after an input
source switch; an unmarked synthetic `Esc` must not close it. A physical `Esc`
is marked by the palette-only Karabiner rule. During marked Japanese text, the
first physical `Esc` cancels composition and keeps the palette open; the next
physical `Esc` closes it.

Generated-rule regression tests also verify that the experimental Column edge
path keeps the direct Arrow last in the initial action list, clears its
direction-specific physical-hold token on key-up, and launches the edge
observer only after the 100ms delay. The observer start creates the token
immediately before spawning its worker, rather than adding a process launch in
front of every direct Arrow key-down.

The generated-rule tests also require exactly one `Shift+j` and one `Shift+k`
Boost mapping. They must send unmodified Finder-native Down/Up Arrow repeat,
apply only in Normal Mode with no confirmed marks or motion count, and start no
helper process. The default unmodified `j/k` mappings continue to use the
token-controlled transient worker and precise AX wrap path.

New-folder mapping checks require one view-independent `n` helper that creates
beside the selected item. Normal `n` may only mark its key-down state; key-up
must clear that state before starting the helper once, so Finder's inline
rename field never overlaps the original physical key press. The checks also
preserve Finder's active-target behavior as an exact `Shift+n` mapping.

Normal Mode `d` mapping checks require a process-free, non-repeating
`Command+Delete` path when confirmed marks are not known to exist. A separate
guarded path keeps the marked-action helper when `s` may have established
confirmed marks, so the transient cursor is not included. Visual Mode deletion
is also required to be non-repeating.

The Column worker uses a dedicated lightweight context containing only the
nearest Column `AXList` and Finder PID. At the 100ms handoff it releases the
continuously held logical Arrow before reading AX state, then sends complete
Arrow down/up taps every 16.667ms. It compares one selected child every 50ms
and does not read the item count, edge range, `AXIndex`, remaining distance,
or scrollbar value. Finder's physical scrolling and the timing of the
Arrow-to-Option+Arrow handoff are covered by the Column dogfood check because
they cannot be represented by the UI-independent self-test.

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

The Column runner temporarily disables Finder grouping for the benchmark run
so group headings cannot be mistaken for fixture items. After closing the
final window, or from its failure cleanup path, it restores the original
grouping criterion and enabled state. Restoration first selects the captured
criterion from the final test window's Group menu, then writes back the exact
two Finder preference values after that window closes.

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
held grouped List movement whose final selection must remain a fixture item, a
Date-grouped Column View whose headings must be skipped by first/last,
counted movement, and held movement, a one-second ungrouped List movement, and
Icon View forward/reverse row wrap. The test temporarily changes grouping only
on its dedicated Finder window and restores the original Finder grouping
criterion and enabled state before closing it.

Run `make test-finder-selection` for the confirmed-mark selection model. It
opens dedicated List, Column, and Icon windows and verifies that A remains
visible after moving to B, that A and B remain visible after moving to C, that
C is still transient and excluded from the copy target, and that one clear
command removes the displayed selection. It marks four items from bottom to
top and in the alternating order `B -> D -> A -> C`, checking the persisted
marks, visible selection, and last-mark anchor after every `s`. Its List case
also holds `j` while A and B are confirmed and checks that both marks remain
visible, exercising the verified fallback rather than the unmarked fast path.
The test uses isolated state files and closes every window it creates.

Run `make test-finder-new-folder` for the Normal Mode `n` target and
same-window contract. It selects a directory in dedicated List, Column, and
Icon windows, then verifies that the new folder is created beside that
directory rather than inside it, that no Finder window is added, and that the
new item is selected with an `AXTextField` focused for inline rename. The test
expands a directory in List View, selects its nested child file, and requires
the new folder to appear beside that child inside the expanded directory
rather than at the Finder window root. The nested path remains in the same
List window and must also enter inline rename. The test
also creates 1,000-item fixtures for all three views, starts from the first
item, and requires the inline rename focus to remain after the long scroll has
settled for another 500ms. It uses isolated fixture directories and closes only
the Finder windows rooted inside those fixtures.

Run `make test-finder-edge-monitor` to exercise the default precise
wrap-and-scroll actions used by List and Column navigation. It opens dedicated
windows on the 1,000-item fixture and verifies both last-to-first and
first-to-last transitions with `down-wrap` and `up-wrap`. It also checks that
the navigation content's vertical scroll value reaches the corresponding
leading or trailing range after each wrap. The List assertions additionally
compare the selected item URL with the URLs in Finder's actual `AXVisibleRows`;
AX element identity is not used because Finder can expose different proxies for
the same row. The List case also checks C-backed `5j`/`5k`, then starts held
repeats from both document edges without reaching a wrap and requires the
viewport to follow the selected row in both directions. It then starts held
repeats near both edges, crosses each boundary, and requires the selected path,
scroll value, and actual visible rows to move to the same opposite range.
The default-off
native Option+Arrow edge-monitor experiment remains covered by its headless
state and lock tests rather than being treated as release behavior. The result
is independent of display refresh rate and physical key repeat.
