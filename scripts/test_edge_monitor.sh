#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
helper="$repo_root/.build/finder_ax_step"
temp_root="$(mktemp -d "$repo_root/.build/edge-monitor-test.XXXXXX")"

cleanup() {
    rm -rf "$temp_root"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

state_root="$temp_root/.local/state/finder-vim"
mkdir -p "$state_root"

"$helper" edge-monitor-self-test
HOME="$temp_root" "$helper" edge-monitor-lock-self-test
HOME="$temp_root" "$helper" hold-token-stop-self-test
HOME="$temp_root" "$helper" vertical-hold-state-self-test

for lock_name in \
        finder_list_down_edge_monitor.lock \
        finder_list_up_edge_monitor.lock \
        finder_column_vertical_edge_monitor.lock; do
    lock_file="$state_root/$lock_name"
    if [[ ! -f "$lock_file" ]]; then
        print -u2 -- "Missing edge monitor lock: $lock_file"
        exit 1
    fi
done

HOME="$temp_root" "$helper" hold-token-start down
HOME="$temp_root" "$helper" hold-token-start up
HOME="$temp_root" "$helper" vertical-edge-monitor-cancel
for direction in down up; do
    token_file="$state_root/finder_${direction}_hold.txt"
    if [[ -s "$token_file" ]]; then
        print -u2 -- "Vertical monitor cancellation left a token: $token_file"
        exit 1
    fi
done

print -- "Vertical edge monitor and hold-token headless tests passed."
