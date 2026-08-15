// Compatibility entry point for the transient navigation worker.
//
// Keep these implementation fragments in one translation unit: the worker's
// hot path relies on internal linkage and whole-file optimization, while the
// responsibility-based files make the implementation navigable.
#include "worker/finder_ax_step/prelude.inc"
#include "worker/finder_ax_step/accessibility.inc"
#include "worker/finder_ax_step/selection.inc"
#include "worker/finder_ax_step/events.inc"
#include "worker/finder_ax_step/movement.inc"
#include "worker/finder_ax_step/runtime.inc"
#include "worker/finder_ax_step/worker.inc"
#include "worker/finder_ax_step/hold_state.inc"
#include "worker/finder_ax_step/hold_repeat.inc"
#include "worker/finder_ax_step/edge_state.inc"
#include "worker/finder_ax_step/edge_monitor.inc"
#include "worker/finder_ax_step/cli.inc"
