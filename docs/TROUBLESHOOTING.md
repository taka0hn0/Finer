# Troubleshooting

Finer is pre-alpha. Start with the current repository commit and include the
environment details listed at the end of this document when reporting a bug.

## Installation checklist

1. Install and configure Karabiner-Elements.
2. Confirm the Xcode Command Line Tools, `jq`, and `ripgrep` are available.
3. Run `make check`.
4. Run `make install`.
5. Open Karabiner-Elements Settings > Complex Modifications.
6. Add and enable both **Finer Navigation** and **Finer Utility Commands**.

The installer places generated artifacts under:

- `~/.local/libexec/finder-vim/`
- `~/.local/state/finder-vim/`
- `~/.config/karabiner/assets/complex_modifications/finder-vim.json`

It does not edit `~/.config/karabiner/karabiner.json`.

## Finer types letters instead of navigating

Check all of the following:

- Finder is the frontmost application.
- Keyboard focus is not inside a search field, rename field, or other text
  input. Finer deliberately bypasses text fields.
- Both Finer complex-modification rules are enabled.
- An older copy of the same rules is not enabled at the same time.

If this started after an update, replace the enabled rule copies as described
below.

## Installed changes do not take effect

`make install` updates the importable rule file. Karabiner keeps the
manipulators that were previously added to the active profile.

After updating:

1. Open Karabiner-Elements Settings > Complex Modifications.
2. Remove the enabled **Finer Navigation** and **Finer Utility Commands** rules.
3. Add both rules again from the newly installed Finer entry.
4. Enable both rules.

Do not delete or replace the complete `karabiner.json`.

## Keys are intercepted but Finder does not move

Finer's native helpers use macOS Accessibility to read and update Finder
selection. Karabiner-Elements also requires the macOS permissions documented
by its installer.

Open System Settings > Privacy & Security and verify the required Accessibility
and Input Monitoring entries are enabled. If macOS shows stale entries after a
binary or application update, toggle the relevant entry off and on, then
restart Finder and Karabiner-Elements.

Also verify the installed helpers exist:

```sh
ls -l ~/.local/libexec/finder-vim/finder_ax_step
ls -l ~/.local/libexec/finder-vim/finder_ax_move
```

## List View folders do not expand with `l`

Finer preserves Finder's native List View left/right behavior. Finder does not
show a disclosure triangle for every folder at every location. This is common
at locations such as the top level of Documents or Downloads, while a nested
folder may expose disclosure triangles normally.

Use `o` to open the selected item. `h` and `l` remain native collapse/expand
motions when Finder supplies a disclosure triangle.

## `Esc` does not clear selection

The first `Esc` in Visual Mode exits Visual Mode and preserves the selected
range. A second `Esc` in Normal Mode asks Finder to deselect all items.

If the second press does nothing:

- confirm **Finer Navigation** is the newly installed version;
- check Accessibility permission;
- remove duplicate or older enabled Finer rules;
- confirm focus is in the Finder item view rather than a text field or dialog.

Finer invokes Finder's Deselect All menu action through Accessibility. It does
not broadcast `Option-Command-A`, so an unrelated global shortcut should not be
triggered.

## Visual Mode counts or `gg`/`G` select unexpected items

Replace the enabled rules after installing the newest snapshot. The active
rules must invoke `finder_ax_move visual-down`, `visual-up`, `visual-first`, and
`visual-last`.

Expected behavior:

- `v 5j` selects the starting item plus five items below it;
- `v 5k` selects the starting item plus five items above it;
- `gg` extends from the original `v` item to the first item;
- `G` extends from the original `v` item to the last item;
- mixing those commands keeps the original `v` item as the anchor.

Press `Esc` once to leave Visual Mode and again to clear Finder selection.

## Discontiguous selection moves from the wrong item

After marking item A with `s`, moving to item B, and marking B with `s`, the
next `h`/`j`/`k`/`l` movement should start from B. If it starts from A, replace
the enabled rules and reinstall the current helpers.

`Esc` clears the mark and navigation-anchor state.

## Held movement is slow, jumps, or continues after key-up

- Disable duplicate Finer rules.
- Confirm only the current helpers are installed.
- Reproduce in a local folder before testing a cloud-backed location.
- Record the Finder view, grouping, item count, and whether the directory
  contains folders, empty files, or realistic mixed content.
- Run `make check` before running a visible benchmark.

Do not use synthetic `CGEventPost` keyboard events as an end-to-end latency
measurement. See [BENCHMARKS.md](BENCHMARKS.md) for the supported procedures.

## A Finer process remains after input stops

The navigation worker is reused during an active burst and exits after 750ms
without commands. Wait at least one second after the last Finer input, then
check:

```sh
pgrep -fl finder_ax_step
```

An idle result should contain no Finer worker. Benchmark commands may enable
temporary metrics and should be allowed to finish before checking.

## Reinstall or uninstall

Installation is repeatable:

```sh
make install
```

Before uninstalling, disable the active Finer rules in Karabiner-Elements, then
run:

```sh
make uninstall
```

The scripts preserve runtime state and back up a locally changed importable
rule under `~/.local/state/finder-vim/backups/` before replacement or removal.

## Information to include in a bug report

Include:

```text
Finer commit or version:
macOS version and build:
Karabiner-Elements version:
Keyboard layout: US / JIS / other
Finder UI language:
Finder view: List / Column / Icon / Desktop
Grouping or sorting:
Approximate item count:
Input sequence:
Expected result:
Actual result:
```

Use synthetic names in reproduction steps. Remove user names, home paths,
private file names, clipboard contents, and unrelated Karabiner configuration
before posting logs or screenshots.
