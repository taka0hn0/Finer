#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
build_dir="$repo_root/.build"
libexec_dir="$HOME/.local/libexec/finder-vim"
state_dir="$HOME/.local/state/finder-vim"
rule_dir="$HOME/.config/karabiner/assets/complex_modifications"

for artifact in finder_ax_step finder_ax_move; do
    if [ ! -x "$build_dir/$artifact" ]; then
        print -u2 -- "Missing build artifact: $build_dir/$artifact"
        print -u2 -- "Run make build first."
        exit 1
    fi
done

mkdir -p "$libexec_dir" "$state_dir" "$rule_dir"

/usr/bin/install -m 0755 "$build_dir/finder_ax_step" "$libexec_dir/finder_ax_step"
/usr/bin/install -m 0755 "$build_dir/finder_ax_move" "$libexec_dir/finder_ax_move"
/usr/bin/install -m 0755 "$repo_root/scripts/finder_action_marked.sh" "$libexec_dir/finder_action_marked.sh"
/usr/bin/install -m 0755 "$repo_root/scripts/finder_paste.sh" "$libexec_dir/finder_paste.sh"
/usr/bin/install -m 0644 "$repo_root/rules/generated/finder-vim.json" "$rule_dir/finder-vim.json"

touch \
    "$state_dir/finder_marks.txt" \
    "$state_dir/finder_copy.txt" \
    "$state_dir/finder_cut.txt" \
    "$state_dir/finder_down_hold.txt" \
    "$state_dir/finder_up_hold.txt" \
    "$state_dir/finder_left_hold.txt" \
    "$state_dir/finder_right_hold.txt"

print -- "Finder Vim development snapshot installed."
print -- "Enable its rules in Karabiner-Elements > Complex Modifications."
