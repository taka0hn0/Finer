#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
build_dir="${FINDER_VIM_BUILD_DIR:-$repo_root/.build}"
libexec_dir="$HOME/.local/libexec/finder-vim"
state_dir="$HOME/.local/state/finder-vim"
rule_dir="$HOME/.config/karabiner/assets/complex_modifications"
rule_file="$rule_dir/finder-vim.json"
source_rule="$repo_root/rules/generated/finder-vim.json"

typeset -a sources=(
    "$build_dir/finder_ax_step"
    "$build_dir/finder_ax_move"
    "$repo_root/scripts/finder_action_marked.sh"
    "$repo_root/scripts/finder_paste.sh"
    "$source_rule"
)
typeset -a destinations=(
    "$libexec_dir/finder_ax_step"
    "$libexec_dir/finder_ax_move"
    "$libexec_dir/finder_action_marked.sh"
    "$libexec_dir/finder_paste.sh"
    "$rule_file"
)
typeset -a modes=(0755 0755 0755 0755 0644)
typeset -a state_names=(
    finder_marks.txt
    finder_copy.txt
    finder_cut.txt
    finder_down_hold.txt
    finder_up_hold.txt
    finder_left_hold.txt
    finder_right_hold.txt
)
typeset -a staged_files=()

fail() {
    print -u2 -- "finer install: $1"
    exit 1
}

cleanup_staged_files() {
    local staged
    for staged in "${staged_files[@]}"; do
        if [[ -n "$staged" ]]; then
            rm -f "$staged"
        fi
    done
}

trap cleanup_staged_files EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for ((index = 1; index <= ${#sources[@]}; ++index)); do
    source_file="${sources[index]}"
    if [[ ! -f "$source_file" || ! -r "$source_file" ]]; then
        fail "missing source artifact: $source_file"
    fi
done
for helper in "$build_dir/finder_ax_step" "$build_dir/finder_ax_move"; do
    if [[ ! -x "$helper" ]]; then
        fail "build artifact is not executable: $helper"
    fi
done

if [[ -L "$rule_file" ]]; then
    fail "refusing to replace symlinked rule: $rule_file"
fi
for state_name in "${state_names[@]}"; do
    state_file="$state_dir/$state_name"
    if [[ -e "$state_file" && ( -L "$state_file" || ! -f "$state_file" ) ]]; then
        fail "state path is not a regular file: $state_file"
    fi
done

mkdir -p "$libexec_dir" "$state_dir" "$rule_dir"

backup_dir=""
if [[ -f "$rule_file" ]] && ! cmp -s "$rule_file" "$source_rule"; then
    backup_dir="$state_dir/backups/$(date -u +%Y%m%dT%H%M%SZ)-install-$$"
    mkdir -p "$backup_dir"
    /usr/bin/install -m 0644 "$rule_file" "$backup_dir/finder-vim.json"
fi

for ((index = 1; index <= ${#sources[@]}; ++index)); do
    destination="${destinations[index]}"
    staged="$destination.finer-new.$$"
    if [[ -e "$staged" ]]; then
        fail "staging path already exists: $staged"
    fi
    staged_files+=("$staged")
    /usr/bin/install -m "${modes[index]}" "${sources[index]}" "$staged"
done

for ((index = 1; index <= ${#destinations[@]}; ++index)); do
    /bin/mv -f "${staged_files[index]}" "${destinations[index]}"
    staged_files[index]=""
done

for state_name in "${state_names[@]}"; do
    state_file="$state_dir/$state_name"
    if [[ ! -e "$state_file" ]]; then
        : > "$state_file"
        chmod 0600 "$state_file"
    fi
done

if [[ -n "$backup_dir" ]]; then
    print -- "Previous importable rule backed up to $backup_dir/finder-vim.json"
fi
print -- "Finer development snapshot installed."
print -- "Enable its rules in Karabiner-Elements > Complex Modifications."
