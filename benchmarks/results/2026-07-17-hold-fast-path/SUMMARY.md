# Held vertical navigation fast path — 2026-07-17

This focused comparison measures sustained vertical movement inside a
1,000-item empty-files fixture on macOS 26.5.2 (25F84), Finder 26.4, and an
arm64 Mac15,12. Each variant ran three one-second repetitions. Finder was sorted
by name; grouping was not controlled.

The retained candidate at the time of this throughput run read the selected
index once, advanced a local predicted index, and directly selected each
destination with a relative minimum 8.333ms interval between repeat starts. It
validated the real Finder selection at release. The baseline performed
selection readback on each step and retained the old 5ms post-step delay.

Subsequent dogfood found that the relative sleep produced less even visual
motion than native Finder repeat. The follow-up direct-AX candidate moved to an
absolute 8.333ms timeline, skipped expired ticks instead of issuing catch-up
writes, and reused the one-item selection array while replacing process-name
lookups with same-PID frontmost checks. Those cadence and hot-loop refinements
are not represented by the throughput values below and require a follow-up
visible measurement.

## Results

Values are nearest-rank percentiles. With three samples, p95 is the maximum.

| View / strategy | Throughput p50 | Throughput p95 | Stop-return p95 | Passed | Drift through 100ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| List / verified baseline | 58.236 steps/s | 59.663 steps/s | 32.292ms | 3/3 | 0/3 |
| List / retained direct AX | 95.756 steps/s | 97.524 steps/s | 36.596ms | 3/3 | 0/3 |
| Column / verified baseline | 23.820 steps/s | 23.848 steps/s | 73.372ms | 3/3 | 0/3 |
| Column / retained direct AX | 32.510 steps/s | 33.581 steps/s | 61.670ms | 3/3 | 0/3 |
| List / temporary native arrow pair at 4ms | 77.119 steps/s | 78.821 steps/s | 59.794ms | 3/3 | 0/3 |
| List / temporary repeated keyDown at 8.333ms | 45.705 steps/s | 46.097 steps/s | 55.477ms | 3/3 | 0/3 |

The direct-AX candidate improved List p50 throughput by 64.4% and Column by 36.5%
against the same-tree verified baseline. Native arrow pairs and autorepeat-
marked keyDown events were slower and had no stop-latency advantage, so both
temporary candidates were removed.

## Correctness and scope

All recorded final paths matched the fixture position derived from the actual
Finder selection, and no selection changed at 25, 50, or 100ms after helper
return. The direct-AX candidate kept the old verified logic whenever
confirmed marks exist, and performs a release-only recovery if a grouped List
header leaves Finder selection empty.

These runs invoke the helper directly and exclude physical input, Karabiner
evaluation, and Finder frame rendering. They do not establish native-equivalent
end-to-end speed or visual smoothness. Grouping was not controlled in this
benchmark; grouped correctness belongs to the dedicated Finder regression.

`outcomes.tsv` contains the sanitized numeric samples used in the table.
`environment.txt` records the common environment and candidate definitions.
