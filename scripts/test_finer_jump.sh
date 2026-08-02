#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
helper="$repo_root/.build/finer_jump"
test_root="$(mktemp -d "$repo_root/.build/finer-jump-test.XXXXXX")"

cleanup() {
    rm -rf "$test_root"
}
trap cleanup EXIT

fail() {
    print -u2 -- "finer jump test: $1"
    exit 1
}

"$helper" --self-test-palette-escape \
    || fail "physical Esc marker gate failed"

mkdir -p \
    "$test_root/Alpha Project" \
    "$test_root/Beta" \
    "$test_root/Gamma Untouched" \
    "$test_root/notes/alpha archive"
touch \
    "$test_root/Alpha Project/Alpha Report.pdf" \
    "$test_root/Beta/meeting notes.txt" \
    "$test_root/notes/alpha archive/alpha draft.md"

fake_zoxide="$test_root/fake zoxide"
print -r -- '#!/bin/zsh
set -euo pipefail
if [[ "${1:-}" == query ]]; then
    print -r -- "100 $FINER_JUMP_TEST_ROOT/Alpha Project"
    print -r -- "80 $FINER_JUMP_TEST_ROOT/Beta"
    print -r -- "60 $FINER_JUMP_TEST_ROOT/notes/alpha archive"
elif [[ "${1:-}" == add ]]; then
    if [[ -n "${FINER_ZOXIDE_ADD_OUTPUT:-}" ]]; then
        print -r -- "$2" >> "$FINER_ZOXIDE_ADD_OUTPUT"
    fi
    exit 0
else
    exit 2
fi' > "$fake_zoxide"
chmod 0755 "$fake_zoxide"

fake_spotlight="$test_root/fake mdfind"
print -r -- '#!/bin/zsh
set -euo pipefail
[[ "$*" == *"-0"* && "$*" == *"-onlyin"* && "$*" == *"-name"* ]] || exit 64
printf "%s\0" \
    "$FINER_JUMP_TEST_ROOT/Alpha Project/Alpha Report.pdf" \
    "$FINER_JUMP_TEST_ROOT/Beta/meeting notes.txt" \
    "$FINER_JUMP_TEST_ROOT/notes/alpha archive/alpha draft.md" \
    "$FINER_JUMP_TEST_ROOT/Alpha Project" \
    "$FINER_JUMP_TEST_ROOT/Gamma Untouched"' > "$fake_spotlight"
chmod 0755 "$fake_spotlight"

fake_ax_move="$test_root/fake finder_ax_move"
print -r -- '#!/bin/zsh
set -euo pipefail
[[ "${1:-}" == jump-to || "${1:-}" == reveal-file ]] || exit 64
[[ -n "${2:-}" ]] || exit 64
printf "%s\t%s\n" "$1" "$2" > "$FINER_JUMP_NAV_OUTPUT"' > "$fake_ax_move"
chmod 0755 "$fake_ax_move"

fake_failing_ax_move="$test_root/failing finder_ax_move"
print -r -- '#!/bin/zsh
exit 1' > "$fake_failing_ax_move"
chmod 0755 "$fake_failing_ax_move"

query() {
    FINER_ZOXIDE_PATH="$fake_zoxide" \
    FINER_JUMP_TEST_ROOT="$test_root" \
        "$helper" --query "$1"
}

file_query() {
    FINER_SPOTLIGHT_PATH="$fake_spotlight" \
    FINER_SPOTLIGHT_ROOT="$test_root" \
    FINER_JUMP_TEST_ROOT="$test_root" \
        "$helper" --query-files "$1"
}

first_default="$(query "" | sed -n '1p')"
[[ "$first_default" == "$test_root/Alpha Project" ]] \
    || fail "empty query did not preserve zoxide frecency order"

beta_result="$(query beta)"
[[ "$beta_result" == "$test_root/Beta" ]] \
    || fail "basename filter did not select Beta"

alpha_results="$(query alpha)"
[[ "$(print -r -- "$alpha_results" | sed -n '1p')" == "$test_root/Alpha Project" ]] \
    || fail "basename prefix did not outrank a later path match"
[[ "$(print -r -- "$alpha_results" | sed -n '2p')" == "$test_root/notes/alpha archive" ]] \
    || fail "second alpha result is missing"

multi_token_result="$(query 'notes archive')"
[[ "$multi_token_result" == "$test_root/notes/alpha archive" ]] \
    || fail "multi-token path filter failed"

alpha_file_results="$(file_query alpha)"
[[ "$(print -r -- "$alpha_file_results" | sed -n '1p')" \
    == "$test_root/Alpha Project/Alpha Report.pdf" ]] \
    || fail "Spotlight file basename priority is incorrect"
[[ "$(print -r -- "$alpha_file_results" | sed -n '2p')" \
    == "$test_root/notes/alpha archive/alpha draft.md" ]] \
    || fail "second Spotlight file result is missing"
[[ "$alpha_file_results" == *"$test_root/Alpha Project"* ]] \
    || fail "Spotlight directory result was not included"

gamma_folder_result="$(file_query gamma)"
[[ "$gamma_folder_result" == "$test_root/Gamma Untouched" ]] \
    || fail "unvisited Spotlight folder was not included"

file_multi_token_result="$(file_query 'alpha draft')"
[[ "$file_multi_token_result" \
    == "$test_root/notes/alpha archive/alpha draft.md" ]] \
    || fail "multi-token file filtering failed"

combined_results="$(
    FINER_ZOXIDE_PATH="$fake_zoxide" \
    FINER_SPOTLIGHT_PATH="$fake_spotlight" \
    FINER_SPOTLIGHT_ROOT="$test_root" \
    FINER_JUMP_TEST_ROOT="$test_root" \
        "$helper" --query-all alpha
)"
[[ "$(print -r -- "$combined_results" | sed -n '1p')" \
    == $'folder\t'"$test_root/Alpha Project" ]] \
    || fail "combined search did not preserve the stronger folder result"
[[ "$combined_results" == *$'file\t'"$test_root/Alpha Project/Alpha Report.pdf"* ]] \
    || fail "combined search did not include files"
alpha_folder_count="$(
    print -r -- "$combined_results" \
        | awk -F '\t' -v path="$test_root/Alpha Project" '$1 == "folder" && $2 == path { count++ } END { print count + 0 }'
)"
[[ "$alpha_folder_count" == 1 ]] \
    || fail "duplicate zoxide and Spotlight folder was not collapsed"

spotlight_only_results="$(
    FINER_ZOXIDE_PATH="$test_root/missing-zoxide" \
    FINER_SPOTLIGHT_PATH="$fake_spotlight" \
    FINER_SPOTLIGHT_ROOT="$test_root" \
    FINER_JUMP_TEST_ROOT="$test_root" \
        "$helper" --query-all gamma
)"
[[ "$spotlight_only_results" == $'folder\t'"$test_root/Gamma Untouched" ]] \
    || fail "Spotlight-only folder search failed without zoxide"

if FINER_ZOXIDE_PATH="$test_root/missing-zoxide" "$helper" --query "" \
    >/dev/null 2>&1; then
    fail "missing zoxide unexpectedly succeeded"
fi

if FINER_SPOTLIGHT_PATH="$test_root/missing-mdfind" \
    FINER_SPOTLIGHT_ROOT="$test_root" \
    "$helper" --query-files alpha >/dev/null 2>&1; then
    fail "missing Spotlight executable unexpectedly succeeded"
fi

navigation_output="$test_root/navigation-output"
FINER_OSASCRIPT_PATH="/usr/bin/false" \
FINER_AX_MOVE_PATH="$fake_ax_move" \
FINER_JUMP_NAV_OUTPUT="$navigation_output" \
    "$helper" --navigate "$test_root/Beta"
[[ "$(<"$navigation_output")" == $'jump-to\t'"$test_root/Beta" ]] \
    || fail "navigation was not delegated to finder_ax_move"

fake_osascript="$test_root/fake osascript"
print -r -- '#!/bin/zsh
set -euo pipefail
if (( $# == 3 )); then
    print -r -- "$3" > "$FINER_JUMP_NAV_OUTPUT"
else
    printf "%s\t%s\n" "$3" "$4" > "$FINER_JUMP_NAV_OUTPUT"
fi' > "$fake_osascript"
chmod 0755 "$fake_osascript"

rm -f "$navigation_output"
FINER_OSASCRIPT_PATH="$fake_osascript" \
FINER_AX_MOVE_PATH="$test_root/missing-finder-ax-move" \
FINER_JUMP_NAV_OUTPUT="$navigation_output" \
    "$helper" --navigate "$test_root/Alpha Project"
[[ "$(<"$navigation_output")" == "$test_root/Alpha Project" ]] \
    || fail "direct navigation did not receive the selected path"

rm -f "$navigation_output"
FINER_OSASCRIPT_PATH="/usr/bin/false" \
FINER_AX_MOVE_PATH="$fake_ax_move" \
FINER_JUMP_NAV_OUTPUT="$navigation_output" \
    "$helper" --navigate-file "$test_root/Beta/meeting notes.txt"
[[ "$(<"$navigation_output")" \
    == $'reveal-file\t'"$test_root/Beta/meeting notes.txt" ]] \
    || fail "file reveal was not delegated to finder_ax_move"

rm -f "$navigation_output"
FINER_OSASCRIPT_PATH="$fake_osascript" \
FINER_AX_MOVE_PATH="$test_root/missing-finder-ax-move" \
FINER_JUMP_NAV_OUTPUT="$navigation_output" \
    "$helper" --navigate-file "$test_root/Alpha Project/Alpha Report.pdf"
[[ "$(<"$navigation_output")" \
    == "$test_root/Alpha Project"$'\t'"$test_root/Alpha Project/Alpha Report.pdf" ]] \
    || fail "direct file reveal did not receive parent and file paths"

learning_output="$test_root/zoxide-add-output"
rm -f "$navigation_output" "$learning_output"
FINER_ZOXIDE_PATH="$fake_zoxide" \
FINER_ZOXIDE_ADD_OUTPUT="$learning_output" \
FINER_OSASCRIPT_PATH="$fake_osascript" \
FINER_AX_MOVE_PATH="$test_root/missing-finder-ax-move" \
FINER_JUMP_NAV_OUTPUT="$navigation_output" \
    "$helper" --navigate-and-learn-folder "$test_root/Gamma Untouched"
[[ "$(<"$navigation_output")" == "$test_root/Gamma Untouched" ]] \
    || fail "unvisited folder navigation used the wrong path"
[[ "$(<"$learning_output")" == "$test_root/Gamma Untouched" ]] \
    || fail "unvisited folder was not added to zoxide after navigation"

rm -f "$navigation_output" "$learning_output"
FINER_ZOXIDE_PATH="$fake_zoxide" \
FINER_ZOXIDE_ADD_OUTPUT="$learning_output" \
FINER_OSASCRIPT_PATH="$fake_osascript" \
FINER_AX_MOVE_PATH="$test_root/missing-finder-ax-move" \
FINER_JUMP_NAV_OUTPUT="$navigation_output" \
    "$helper" --navigate-and-learn-file "$test_root/Beta/meeting notes.txt"
[[ "$(<"$learning_output")" == "$test_root/Beta" ]] \
    || fail "file navigation did not add its parent folder to zoxide"

rm -f "$learning_output"
if FINER_ZOXIDE_PATH="$fake_zoxide" \
    FINER_ZOXIDE_ADD_OUTPUT="$learning_output" \
    FINER_OSASCRIPT_PATH="/usr/bin/false" \
    FINER_AX_MOVE_PATH="$fake_failing_ax_move" \
    "$helper" --navigate-and-learn-folder "$test_root/Gamma Untouched" \
    >/dev/null 2>&1; then
    fail "failed folder navigation unexpectedly succeeded"
fi
[[ ! -e "$learning_output" ]] \
    || fail "failed folder navigation was added to zoxide"

print -- "Finer Jump headless tests passed."
