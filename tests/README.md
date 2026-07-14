# Tests

The initial extraction relies on `make check` for build, JSON, shell syntax,
and personal-path checks.

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

Repeat in List and Column views, with no deliberate delay and with a directory
containing many items.

