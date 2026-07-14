#!/bin/zsh
set -euo pipefail

libexec_dir="$HOME/.local/libexec/finder-vim"
rule_file="$HOME/.config/karabiner/assets/complex_modifications/finder-vim.json"

rm -f \
    "$libexec_dir/finder_ax_step" \
    "$libexec_dir/finder_ax_move" \
    "$libexec_dir/finder_action_marked.sh" \
    "$libexec_dir/finder_paste.sh" \
    "$rule_file"
rmdir "$libexec_dir" 2>/dev/null || true

print -- "Finder Vim executables and the importable rule were removed."
print -- "User configuration and state were preserved."

