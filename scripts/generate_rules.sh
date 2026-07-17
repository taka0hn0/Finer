#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
source_dir="${FINER_RULE_SOURCE_DIR:-$repo_root/rules/source}"
output="${FINER_RULE_OUTPUT:-$repo_root/rules/generated/finder-vim.json}"
mode=write

fail() {
    print -u2 -- "finer rules: $1"
    exit 1
}

if (( $# > 1 )); then
    print -u2 -- "Usage: $0 [--check]"
    exit 64
fi
if (( $# == 1 )); then
    if [[ "$1" != --check ]]; then
        print -u2 -- "Usage: $0 [--check]"
        exit 64
    fi
    mode=check
fi

utility_source="$source_dir/finer-utility-commands.json"
navigation_source="$source_dir/finer-navigation.json"
for source_file in "$utility_source" "$navigation_source"; do
    if [[ ! -f "$source_file" || ! -r "$source_file" || -L "$source_file" ]]; then
        fail "source must be a readable regular file: $source_file"
    fi
done
if [[ -L "$output" ]]; then
    fail "refusing symlinked output: $output"
fi
mkdir -p "${output:h}"

temp_output="$(mktemp "${output:h}/.finder-vim.json.XXXXXX")"
cleanup() {
    rm -f "$temp_output"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

jq -n \
    --slurpfile utility "$utility_source" \
    --slurpfile navigation "$navigation_source" '
    if ($utility | length) != 1
        or ($navigation | length) != 1
        or $utility[0].description != "Finer Utility Commands"
        or $navigation[0].description != "Finer Navigation"
    then
        error("invalid Finer rule source modules")
    else
        {
            title: "Finer (development snapshot)",
            rules: [$utility[0], $navigation[0]]
        }
    end
' > "$temp_output"
jq empty "$temp_output"
chmod 0644 "$temp_output"

if [[ "$mode" == check ]]; then
    if [[ ! -f "$output" ]] || ! cmp -s "$temp_output" "$output"; then
        fail "generated rule is stale; run make rules"
    fi
    print -- "Generated rule matches source modules."
    exit 0
fi

/bin/mv -f "$temp_output" "$output"
temp_output=""
print -- "Generated $output"
