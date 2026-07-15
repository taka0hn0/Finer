# Worker idle timeout decision run

Date: 2026-07-16 (Asia/Tokyo)

This run compares Finer's five permitted worker idle timeouts using two `j`
commands in a ten-item, empty-files List View fixture. Each timeout was tested
against 100, 400, 700, 1100, and 1600ms submission gaps with ten repetitions,
for 250 command pairs and 500 command records.

## Result

| Timeout | Reused gaps | Processes / 50 pairs | Total CPU | Wakeups | Max footprint | Final idle residency |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 300ms | 100ms | 90 | 8.662ms | 0 | 2,589,104 bytes | 27.116s |
| 500ms | 100, 400ms | 80 | 7.675ms | 0 | 2,605,488 bytes | 40.114s |
| 750ms | 100, 400, 700ms | 70 | 6.637ms | 0 | 2,589,104 bytes | 52.600s |
| 1000ms | 100, 400, 700ms | 70 | 6.676ms | 0 | 2,589,104 bytes | 70.102s |
| 1500ms | 100, 400, 700, 1100ms | 60 | 5.523ms | 0 | 2,589,104 bytes | 90.086s |

All 250 pairs passed. No metrics records were dropped. Reuse was deterministic
for every candidate/gap pair: every one of the ten repetitions either reused a
single process or created two processes.

750ms is the selected default. It reused the 700ms gap that 500ms did not, and
matched the process count of 1000ms while reducing post-command residency by
17.502 seconds across the 50 pairs. The additional reuse at 1500ms saved ten
process creations but increased residency by 37.486 seconds relative to 750ms.

## Files

- `metrics.tsv`: 500 per-command records, including cold/warm state, CPU,
  wakeups, footprint, AX operations, and worker exit delay.
- `outcomes.tsv`: pair-level process reuse, exit delay, and final-selection
  validation. Repository paths are replaced with `$REPO_ROOT`.
- `environment.txt`: commit, helper checksum, OS, Finder, Karabiner, hardware,
  fixture, and matrix metadata. Local home and repository paths are replaced
  with `$HOME` and `$REPO_ROOT`.

The environment reported `dirty=true` because an unrelated untracked
documentation file was present. The tracked implementation and installed
helper matched commit `33d38891c2ee6c61db066bc4a3ef79960d08198f`; the helper
SHA-256 is recorded in `environment.txt`.
