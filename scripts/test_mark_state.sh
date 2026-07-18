#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
test_root="$(mktemp -d "$repo_root/.build/mark-state-test.XXXXXX")"

cleanup() {
    rm -rf "$test_root"
}
trap cleanup EXIT INT TERM

marks_file="$test_root/finder_marks.txt"
anchor_file="$test_root/finder_navigation_anchor.txt"
copy_file="$test_root/finder_copy.txt"
cut_file="$test_root/finder_cut.txt"
marked_path="$test_root/marked item.txt"

print -r -- "$marked_path" > "$marks_file"
print -r -- "1\t7\tfile:///marked%20item.txt" > "$anchor_file"
print -r -- "stale copy" > "$copy_file"
print -r -- "stale cut" > "$cut_file"

KARABINER_FINDER_MARKS_FILE="$marks_file" \
KARABINER_FINDER_ANCHOR_FILE="$anchor_file" \
KARABINER_FINDER_COPY_FILE="$copy_file" \
KARABINER_FINDER_CUT_FILE="$cut_file" \
    "$repo_root/scripts/finder_action_marked.sh" copy

[[ "$(<"$copy_file")" == "$marked_path" ]] || {
    print -u2 -- "mark state test: copy state did not use the confirmed mark"
    exit 1
}
[[ ! -s "$cut_file" ]] || {
    print -u2 -- "mark state test: copy did not clear stale cut state"
    exit 1
}
[[ "$(<"$marks_file")" == "$marked_path" ]] || {
    print -u2 -- "mark state test: copy consumed marks before the rule could clear them"
    exit 1
}
[[ -s "$anchor_file" ]] || {
    print -u2 -- "mark state test: copy unexpectedly cleared the cursor anchor"
    exit 1
}

print -r -- "stale copy" > "$copy_file"

KARABINER_FINDER_MARKS_FILE="$marks_file" \
KARABINER_FINDER_ANCHOR_FILE="$anchor_file" \
KARABINER_FINDER_COPY_FILE="$copy_file" \
KARABINER_FINDER_CUT_FILE="$cut_file" \
    "$repo_root/scripts/finder_action_marked.sh" cut

[[ ! -s "$marks_file" ]] || {
    print -u2 -- "mark state test: marks were not cleared after cut"
    exit 1
}
[[ ! -s "$anchor_file" ]] || {
    print -u2 -- "mark state test: navigation anchor was not cleared after cut"
    exit 1
}
[[ ! -s "$copy_file" ]] || {
    print -u2 -- "mark state test: stale copy state was not cleared"
    exit 1
}
[[ "$(<"$cut_file")" == "$marked_path" ]] || {
    print -u2 -- "mark state test: cut state did not preserve the marked path"
    exit 1
}

print -- "Mark state tests passed."
