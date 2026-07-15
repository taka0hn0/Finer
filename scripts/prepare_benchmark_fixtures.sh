#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
destination="${1:-$repo_root/.build/benchmark-fixtures/column-race}"
counts=(10 1000 10000)

mkdir -p "$destination"

for count in "${counts[@]}"; do
    case_dir="$destination/items-$count"
    child_dir="$case_dir/01-A"
    mkdir -p "$child_dir"
    touch "$case_dir/00-start.txt" "$case_dir/02-sibling.txt"

    typeset -a batch=()
    for ((index = 0; index < count; ++index)); do
        printf -v filename 'item-%05d.txt' "$index"
        batch+=("$child_dir/$filename")
        if (( ${#batch[@]} == 200 )); then
            touch "${batch[@]}"
            batch=()
        fi
    done
    if (( ${#batch[@]} > 0 )); then
        touch "${batch[@]}"
    fi

    actual_count=$(find "$child_dir" -maxdepth 1 -type f | wc -l | tr -d ' ')
    if [[ "$actual_count" != "$count" ]]; then
        print -u2 -- "Unexpected item count in $child_dir: $actual_count"
        print -u2 -- "Remove stale files from this generated fixture and retry."
        exit 1
    fi
    print -- "Prepared $case_dir ($count child items)"
done
