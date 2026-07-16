#!/bin/zsh
set -euo pipefail

if (( $# != 2 )); then
    print -u2 -- "Usage: $0 GIT_REF_OR_WORKTREE LABEL"
    exit 64
fi

repo_root="${0:A:h:h}"
requested_ref="$1"
label="$2"
output_root="${FINDER_VIM_COMPARISON_ROOT:-$repo_root/.build/benchmark-helpers}"
source_path="src/finder_ax_step.c"

if [[ ! "$label" =~ '^[a-z0-9][a-z0-9._-]*$' ]]; then
    print -u2 -- "Invalid comparison helper label: $label"
    exit 64
fi
if [[ -z "$requested_ref" ]]; then
    print -u2 -- "Comparison source ref must not be empty."
    exit 64
fi

mkdir -p "$repo_root/.build" "$output_root/$label"
temp_root="$(mktemp -d "$repo_root/.build/comparison-helper.XXXXXX")"
trap 'rm -rf "$temp_root"' EXIT
source_copy="$temp_root/finder_ax_step.c"
helper_copy="$temp_root/finder_ax_step"
manifest_copy="$temp_root/environment.txt"

head_commit="$(git -C "$repo_root" rev-parse HEAD)"
if [[ "$requested_ref" == WORKTREE ]]; then
    source_kind=worktree
    resolved_commit="$head_commit"
    source_blob=WORKTREE
    cp "$repo_root/$source_path" "$source_copy"
else
    source_kind=git
    if ! resolved_commit="$(git -C "$repo_root" rev-parse --verify "$requested_ref^{commit}" 2>/dev/null)"; then
        print -u2 -- "Git ref does not resolve to a commit: $requested_ref"
        exit 1
    fi
    if ! source_blob="$(git -C "$repo_root" rev-parse --verify "$requested_ref:$source_path" 2>/dev/null)"; then
        print -u2 -- "Missing $source_path at Git ref: $requested_ref"
        exit 1
    fi
    git -C "$repo_root" show "$requested_ref:$source_path" > "$source_copy"
fi

compiler="$(xcrun --find clang)"
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror \
    -framework ApplicationServices -framework Carbon \
    "$source_copy" -o "$helper_copy"

source_sha256="$(shasum -a 256 "$source_copy" | awk '{ print $1 }')"
helper_sha256="$(shasum -a 256 "$helper_copy" | awk '{ print $1 }')"
compiler_version="$("$compiler" --version | awk 'NR == 1 { print; exit }')"
{
    print -- "label=$label"
    print -- "requested_ref=$requested_ref"
    print -- "source_kind=$source_kind"
    print -- "resolved_commit=$resolved_commit"
    print -- "source_blob=$source_blob"
    print -- "source_path=$source_path"
    print -- "source_sha256=$source_sha256"
    print -- "helper_sha256=$helper_sha256"
    print -- "architecture=$(uname -m)"
    print -- "compiler=$compiler"
    print -- "compiler_version=$compiler_version"
    print -- "compile_flags=-std=c11 -O2 -Wall -Wextra -Werror"
    print -- "frameworks=ApplicationServices Carbon"
} > "$manifest_copy"

mv "$helper_copy" "$output_root/$label/finder_ax_step"
mv "$manifest_copy" "$output_root/$label/environment.txt"
print -- "Built $label from $requested_ref"
print -- "Helper: $output_root/$label/finder_ax_step"
print -- "Manifest: $output_root/$label/environment.txt"
