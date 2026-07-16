#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
install_script="$repo_root/scripts/install.sh"
uninstall_script="$repo_root/scripts/uninstall.sh"
test_root="$(mktemp -d "$repo_root/.build/install-test.XXXXXX")"
test_home="$test_root/home with spaces"
expected_main="$test_root/expected-karabiner.json"
expected_original_rule="$test_root/expected-original-rule.json"
expected_modified_rule="$test_root/expected-modified-rule.json"

cleanup() {
    rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
    print -u2 -- "installation test: $1"
    exit 1
}

assert_content() {
    local path="$1"
    local expected="$2"
    [[ -f "$path" ]] || fail "missing file: $path"
    [[ "$(<"$path")" == "$expected" ]] \
        || fail "unexpected content: $path"
}

backup_files() {
    print -rl -- "$test_home"/.local/state/finder-vim/backups/*/finder-vim.json(N)
}

rule_dir="$test_home/.config/karabiner/assets/complex_modifications"
state_dir="$test_home/.local/state/finder-vim"
libexec_dir="$test_home/.local/libexec/finder-vim"
rule_file="$rule_dir/finder-vim.json"
main_config="$test_home/.config/karabiner/karabiner.json"
mkdir -p "$rule_dir" "$state_dir"

print -r -- '{"sentinel":"main-config-must-not-change"}' > "$main_config"
cp "$main_config" "$expected_main"
print -r -- '{"title":"pre-existing-importable-rule"}' > "$rule_file"
cp "$rule_file" "$expected_original_rule"
print -r -- 'preserve marks' > "$state_dir/finder_marks.txt"
print -r -- 'preserve anchor' > "$state_dir/finder_navigation_anchor.txt"
print -r -- 'preserve visual anchor' > "$state_dir/finder_visual_anchor.txt"
print -r -- 'preserve copy' > "$state_dir/finder_copy.txt"
print -r -- 'preserve cut' > "$state_dir/finder_cut.txt"

HOME="$test_home" "$install_script" >/dev/null

for artifact in finder_ax_step finder_ax_move finder_action_marked.sh finder_paste.sh; do
    [[ -x "$libexec_dir/$artifact" ]] || fail "artifact is not executable: $artifact"
    [[ "$(stat -f '%Lp' "$libexec_dir/$artifact")" == 755 ]] \
        || fail "unexpected executable mode: $artifact"
done
[[ "$(stat -f '%Lp' "$rule_file")" == 644 ]] \
    || fail "unexpected rule mode"
cmp -s "$repo_root/.build/finder_ax_step" "$libexec_dir/finder_ax_step" \
    || fail "finder_ax_step differs from build"
cmp -s "$repo_root/.build/finder_ax_move" "$libexec_dir/finder_ax_move" \
    || fail "finder_ax_move differs from build"
cmp -s "$repo_root/rules/generated/finder-vim.json" "$rule_file" \
    || fail "installed rule differs from source"
cmp -s "$main_config" "$expected_main" || fail "main karabiner.json changed"
assert_content "$state_dir/finder_marks.txt" 'preserve marks'
assert_content "$state_dir/finder_navigation_anchor.txt" 'preserve anchor'
assert_content "$state_dir/finder_visual_anchor.txt" 'preserve visual anchor'
assert_content "$state_dir/finder_copy.txt" 'preserve copy'
assert_content "$state_dir/finder_cut.txt" 'preserve cut'
for direction in down up left right; do
    [[ -f "$state_dir/finder_${direction}_hold.txt" ]] \
        || fail "missing hold state: $direction"
done

typeset -a backups=("${(@f)$(backup_files)}")
(( ${#backups[@]} == 1 )) || fail "expected one install backup"
cmp -s "${backups[1]}" "$expected_original_rule" \
    || fail "install backup did not preserve previous rule"

HOME="$test_home" "$install_script" >/dev/null
typeset -a second_backups=("${(@f)$(backup_files)}")
(( ${#second_backups[@]} == 1 )) \
    || fail "idempotent reinstall created another backup"

installed_hash="$(shasum -a 256 "$libexec_dir/finder_ax_step" | awk '{ print $1 }')"
mkdir -p "$test_root/missing-build"
if HOME="$test_home" FINDER_VIM_BUILD_DIR="$test_root/missing-build" \
    "$install_script" >/dev/null 2>&1; then
    fail "install unexpectedly succeeded with missing artifacts"
fi
after_failure_hash="$(shasum -a 256 "$libexec_dir/finder_ax_step" | awk '{ print $1 }')"
[[ "$after_failure_hash" == "$installed_hash" ]] \
    || fail "failed preflight changed the existing install"

print -r -- '{"title":"locally-modified-importable-rule"}' > "$rule_file"
cp "$rule_file" "$expected_modified_rule"
HOME="$test_home" "$uninstall_script" >/dev/null

for artifact in finder_ax_step finder_ax_move finder_action_marked.sh finder_paste.sh; do
    [[ ! -e "$libexec_dir/$artifact" ]] \
        || fail "artifact remains after uninstall: $artifact"
done
[[ ! -e "$rule_file" ]] || fail "rule remains after uninstall"
cmp -s "$main_config" "$expected_main" || fail "main karabiner.json changed"
assert_content "$state_dir/finder_marks.txt" 'preserve marks'
assert_content "$state_dir/finder_navigation_anchor.txt" 'preserve anchor'
assert_content "$state_dir/finder_visual_anchor.txt" 'preserve visual anchor'
assert_content "$state_dir/finder_copy.txt" 'preserve copy'
assert_content "$state_dir/finder_cut.txt" 'preserve cut'

typeset -a final_backups=("${(@f)$(backup_files)}")
(( ${#final_backups[@]} == 2 )) \
    || fail "expected install and uninstall backups"
modified_backup_found=false
for backup in "${final_backups[@]}"; do
    if cmp -s "$backup" "$expected_modified_rule"; then
        modified_backup_found=true
    fi
done
[[ "$modified_backup_found" == true ]] \
    || fail "modified rule was not backed up before uninstall"

HOME="$test_home" "$uninstall_script" >/dev/null
typeset -a repeated_uninstall_backups=("${(@f)$(backup_files)}")
(( ${#repeated_uninstall_backups[@]} == 2 )) \
    || fail "repeated uninstall changed backups"

print -- "Installation integration tests passed."
