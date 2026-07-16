#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
libexec_dir="$HOME/.local/libexec/finder-vim"
state_dir="$HOME/.local/state/finder-vim"
rule_file="$HOME/.config/karabiner/assets/complex_modifications/finder-vim.json"
source_rule="$repo_root/rules/generated/finder-vim.json"

if [[ -L "$rule_file" ]]; then
    print -u2 -- "finer uninstall: refusing to remove symlinked rule: $rule_file"
    exit 1
fi

backup_file=""
if [[ -f "$rule_file" && -f "$source_rule" ]] \
    && ! cmp -s "$rule_file" "$source_rule"; then
    backup_dir="$state_dir/backups/$(date -u +%Y%m%dT%H%M%SZ)-uninstall-$$"
    mkdir -p "$backup_dir"
    backup_file="$backup_dir/finder-vim.json"
    /usr/bin/install -m 0644 "$rule_file" "$backup_file"
fi

rm -f \
    "$libexec_dir/finder_ax_step" \
    "$libexec_dir/finder_ax_move" \
    "$libexec_dir/finder_action_marked.sh" \
    "$libexec_dir/finder_paste.sh" \
    "$rule_file"
rmdir "$libexec_dir" 2>/dev/null || true

if [[ -n "$backup_file" ]]; then
    print -- "Modified importable rule backed up to $backup_file"
fi
print -- "Finer executables and the importable rule were removed."
print -- "User configuration and state were preserved."
