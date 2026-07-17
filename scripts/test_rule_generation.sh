#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
temp_root="$(mktemp -d "${TMPDIR:-/tmp}/finer-rule-generation.XXXXXX")"
cleanup() {
    rm -rf "$temp_root"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

source_dir="$temp_root/source"
output="$temp_root/generated/finder-vim.json"
mkdir -p "$source_dir" "${output:h}"
/bin/cp "$repo_root/rules/source/finer-utility-commands.json" "$source_dir/"
/bin/cp "$repo_root/rules/source/finer-navigation.json" "$source_dir/"
/bin/cp "$repo_root/rules/generated/finder-vim.json" "$output"

run_generator() {
    FINER_RULE_SOURCE_DIR="$source_dir" \
    FINER_RULE_OUTPUT="$output" \
        "$repo_root/scripts/generate_rules.sh" "$@"
}

run_generator --check >/dev/null

changed_source="$temp_root/changed-navigation.json"
jq '.description = "Changed Navigation"' \
    "$source_dir/finer-navigation.json" > "$changed_source"
/bin/mv -f "$changed_source" "$source_dir/finer-navigation.json"
if run_generator --check >/dev/null 2>&1; then
    print -u2 -- "Stale generated rule unexpectedly passed"
    exit 1
fi
if run_generator >/dev/null 2>&1; then
    print -u2 -- "Invalid source description unexpectedly generated"
    exit 1
fi

/bin/cp "$repo_root/rules/source/finer-navigation.json" \
    "$source_dir/finer-navigation.json"
run_generator >/dev/null
run_generator --check >/dev/null
if ! cmp -s "$repo_root/rules/generated/finder-vim.json" "$output"; then
    print -u2 -- "Regenerated rule differs from the tracked snapshot"
    exit 1
fi

/bin/rm -f "$output"
ln -s "$temp_root/symlink-target.json" "$output"
if run_generator >/dev/null 2>&1; then
    print -u2 -- "Symlinked generated output unexpectedly passed"
    exit 1
fi

print -- "Rule generation tests passed."
