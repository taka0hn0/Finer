# Contributing to Finer

Finer is pre-alpha. Contributions are welcome, but navigation behavior,
packaging, and performance constraints are still being established.

## Before starting

For bug fixes, search existing Issues and include a minimal reproduction. For a
new feature, architecture change, or new resident component, open an Issue
before writing a large patch. This avoids work that conflicts with Finer's
performance and idle-resource requirements.

Security vulnerabilities must follow [SECURITY.md](SECURITY.md) and must not be
reported in a public Issue.

## Development setup

Requirements:

- macOS
- Xcode Command Line Tools
- Karabiner-Elements
- `jq`
- `ripgrep`

Build and run checks with:

```sh
make build
make check
make test-install
```

`make check` does not open Finder. The install test uses an isolated temporary
home and does not modify the user's Karabiner configuration.

Finder integration tests and benchmarks are intentionally separate because
they change the visible Finder window and selection. Read
[docs/BENCHMARKS.md](docs/BENCHMARKS.md) before running them.

## Project rules

- Treat [docs/FINDER_VIM_SPEC.md](docs/FINDER_VIM_SPEC.md) as the source of
  truth for product requirements and architecture.
- Update the specification and its Decision Log when a behavior or
  architecture decision changes.
- Preserve zero dedicated resident processes while idle.
- Do not add polling, telemetry, or per-item Accessibility calls to the common
  navigation path.
- Do not edit or publish a user's complete `karabiner.json`.
- Keep personal paths, file names, clipboard contents, and unsanitized
  benchmark data out of commits and Issues.
- Keep the generated Karabiner rule and its regression assertions in sync.
- Use `Finer` as the product name. Existing `finder-vim` paths and
  `FINDER_VIM_*` environment variables are compatibility identifiers.

Edit Karabiner rule objects under `rules/source/`, run `make rules`, and commit
the matching `rules/generated/finder-vim.json` snapshot. `make check-rules`
fails if the source modules and generated importable rule differ.

## Changing navigation or selection

Changes to `src/finder_ax_step.c`, `src/finder_ax_move.swift`, or
`rules/source/*.json` should include the narrowest relevant
regression test. At minimum, verify:

```sh
make check
make test-install
```

If a change can affect latency, held keys, tap bursts, AX request counts, or
worker lifetime, record a before-and-after benchmark under identical
conditions. Do not present helper-only timing as physical-key latency.

Visible Finder testing should cover the affected view among List, Column, and
Icon. Record view grouping, item count, macOS version, Karabiner version, and
the tested commit without recording private file names.

## Documentation changes

Public documentation is written in English. Keep commands copyable, use
relative repository links, and distinguish measured behavior from targets or
plans. Do not advertise unmeasured RAM, CPU, or latency values.

## Pull requests

Keep each pull request focused. Include:

- the user-visible problem;
- the implementation approach;
- tests run and their results;
- before-and-after measurements for performance changes;
- manual Finder coverage, when applicable;
- documentation or Decision Log updates, when applicable.

Before requesting review:

```sh
make check
make test-install
make test-dist
git diff --check
```

The CI workflow runs the first three commands on a GitHub-hosted macOS runner.
