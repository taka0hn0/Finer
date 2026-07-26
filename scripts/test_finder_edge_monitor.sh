#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
fixture_dir="$fixture_root/items-1000/01-A"
helper="${FINDER_VIM_HELPER:-$repo_root/.build/finder_ax_step}"
test_root="$(mktemp -d "$repo_root/.build/finder-edge-monitor-test.XXXXXX")"
marks_file="$test_root/finder_marks.txt"
anchor_file="$test_root/finder_navigation_anchor.txt"
window_id=""

: > "$marks_file"
: > "$anchor_file"

fail() {
    print -u2 -- "Finder edge monitor regression: $1"
    exit 1
}

close_test_window() {
    if [[ -z "$window_id" ]]; then
        return
    fi
    /usr/bin/osascript \
        -e 'on run argv' \
        -e 'set windowId to (item 1 of argv) as integer' \
        -e 'tell application "Finder"' \
        -e 'if exists (first Finder window whose id is windowId) then close (first Finder window whose id is windowId)' \
        -e 'end tell' \
        -e 'end run' -- "$window_id" >/dev/null 2>&1 || true
    window_id=""
    sleep 0.5
}

cleanup() {
    close_test_window
    rm -rf "$test_root"
}
trap cleanup EXIT INT TERM

activate_test_window() {
    /usr/bin/osascript \
        -e 'on run argv' \
        -e 'set windowId to (item 1 of argv) as integer' \
        -e 'tell application "Finder"' \
        -e 'set testWindow to first Finder window whose id is windowId' \
        -e 'set index of testWindow to 1' \
        -e 'activate' \
        -e 'end tell' \
        -e 'end run' -- "$window_id" >/dev/null
}

open_test_window() {
    local view="$1"
    window_id="$(
        /usr/bin/osascript \
            -e 'on run argv' \
            -e 'set fixturePath to item 1 of argv' \
            -e 'set viewName to item 2 of argv' \
            -e 'tell application "Finder"' \
            -e 'set targetFolder to POSIX file fixturePath as alias' \
            -e 'set initialItem to POSIX file (fixturePath & "/item-00000.txt") as alias' \
            -e 'set testWindow to make new Finder window to targetFolder' \
            -e 'if viewName is "list" then' \
            -e 'set current view of testWindow to list view' \
            -e 'set sort column of list view options of testWindow to name column' \
            -e 'else' \
            -e 'set current view of testWindow to column view' \
            -e 'end if' \
            -e 'set selection to {initialItem}' \
            -e 'set index of testWindow to 1' \
            -e 'activate' \
            -e 'return id of testWindow' \
            -e 'end tell' \
            -e 'end run' -- "$fixture_dir" "$view"
    )"
    sleep 1
    activate_test_window
}

run_helper() {
    KARABINER_FINDER_MARKS_FILE="$marks_file" \
    KARABINER_FINDER_ANCHOR_FILE="$anchor_file" \
        "$helper" "$@"
}

selected_path() {
    activate_test_window
    /usr/bin/osascript \
        -e 'tell application "Finder"' \
        -e 'set selectedItems to get selection' \
        -e 'if (count of selectedItems) is 0 then return ""' \
        -e 'return POSIX path of (item 1 of selectedItems as alias)' \
        -e 'end tell'
}

wait_for_path() {
    local expected="$1"
    local actual=""
    for _ in {1..100}; do
        actual="$(selected_path)"
        [[ "$actual" == "$expected" ]] && return 0
        sleep 0.02
    done
    print -u2 -- "expected path=$expected actual=$actual"
    return 1
}

run_view_case() {
    local view="$1"
    print -- "Testing Finder $view default edge wrap action..."
    open_test_window "$view"

    run_helper last >/dev/null
    wait_for_path "$fixture_dir/item-00999.txt" \
        || fail "$view did not reach the last item"
    local wrapped_position
    wrapped_position="$(run_helper down-wrap)"
    [[ "$wrapped_position" =~ '^[1-9][0-9]*$' ]] \
        || fail "$view downward wrap action failed: $wrapped_position"
    wait_for_path "$fixture_dir/item-00000.txt" \
        || fail "$view did not wrap to the first item"

    wrapped_position="$(run_helper up-wrap)"
    [[ "$wrapped_position" =~ '^[1-9][0-9]*$' ]] \
        || fail "$view upward wrap action failed: $wrapped_position"
    wait_for_path "$fixture_dir/item-00999.txt" \
        || fail "$view did not wrap to the last item"

    close_test_window
    print -- "Finder $view default edge wrap action passed."
}

[[ -x "$helper" ]] || fail "missing executable helper: $helper"
[[ -f "$fixture_dir/item-00000.txt"
    && -f "$fixture_dir/item-00999.txt" ]] \
    || fail "missing 1000-item fixture: $fixture_dir"

for view in list column; do
    run_view_case "$view"
done

print -- "Finder default edge wrap regressions passed."
