#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
helper="${FINDER_VIM_MOVE_HELPER:-$repo_root/.build/finder_ax_move}"
test_root="$(mktemp -d "$repo_root/.build/finder-new-folder-test.XXXXXX")"
window_id=""

fail() {
    print -u2 -- "Finder new-folder regression: $1"
    exit 1
}

close_test_windows() {
    /usr/bin/osascript \
        -e 'on run argv' \
        -e 'set testRoot to item 1 of argv' \
        -e 'tell application "Finder"' \
        -e 'repeat with finderWindow in (get Finder windows)' \
        -e 'try' \
        -e 'set targetPath to POSIX path of (target of finderWindow as alias)' \
        -e 'if targetPath starts with testRoot then close finderWindow' \
        -e 'end try' \
        -e 'end repeat' \
        -e 'end tell' \
        -e 'end run' -- "$test_root" >/dev/null 2>&1 || true
    window_id=""
}

cleanup() {
    close_test_windows
    rm -rf "$test_root"
}
trap cleanup EXIT INT TERM

finder_window_count() {
    /usr/bin/osascript \
        -e 'tell application "Finder" to return count of Finder windows'
}

open_test_window() {
    local fixture_dir="$1"
    local selected_dir="$2"
    local view="$3"
    window_id="$(
        /usr/bin/osascript \
            -e 'on run argv' \
            -e 'set fixturePath to item 1 of argv' \
            -e 'set selectedPath to item 2 of argv' \
            -e 'set viewName to item 3 of argv' \
            -e 'tell application "Finder"' \
            -e 'set testWindow to make new Finder window to (POSIX file fixturePath as alias)' \
            -e 'if viewName is "list" then' \
            -e 'set current view of testWindow to list view' \
            -e 'set sort column of list view options of testWindow to name column' \
            -e 'else if viewName is "column" then' \
            -e 'set current view of testWindow to column view' \
            -e 'else' \
            -e 'set current view of testWindow to icon view' \
            -e 'set arrangement of icon view options of testWindow to arranged by name' \
            -e 'end if' \
            -e 'set selection to {POSIX file selectedPath as alias}' \
            -e 'set index of testWindow to 1' \
            -e 'activate' \
            -e 'return id of testWindow' \
            -e 'end tell' \
            -e 'end run' -- "$fixture_dir" "$selected_dir" "$view"
    )"
    sleep 0.5
}

selected_path() {
    /usr/bin/osascript \
        -e 'tell application "Finder"' \
        -e 'return POSIX path of (item 1 of (get selection) as alias)' \
        -e 'end tell' 2>/dev/null
}

focused_role() {
    /usr/bin/osascript \
        -e 'tell application "System Events"' \
        -e 'tell process "Finder"' \
        -e 'set focusedElement to value of attribute "AXFocusedUIElement"' \
        -e 'return value of attribute "AXRole" of focusedElement' \
        -e 'end tell' \
        -e 'end tell' 2>/dev/null
}

run_view_case() {
    local view="$1"
    local item_count="${2:-0}"
    local case_name="${3:-same-window}"
    local fixture_dir="$test_root/$view-$case_name"
    local selected_dir="$fixture_dir/00000-selected-dir"
    mkdir -p "$selected_dir"
    touch "$selected_dir/child.txt" "$fixture_dir/sibling.txt"
    local index
    for ((index = 1; index <= item_count; index++)); do
        touch "$fixture_dir/$(printf '%05d-item.txt' "$index")"
    done

    print -- "Testing Finder $view $case_name new folder..."
    open_test_window "$fixture_dir" "$selected_dir" "$view"
    local window_count_before
    window_count_before="$(finder_window_count)"

    "$helper" new-folder-current-level >/dev/null

    local created_dir=""
    local actual_selection=""
    local actual_role=""
    for _ in {1..100}; do
        created_dir="$(
            find "$fixture_dir" \
                -mindepth 1 \
                -maxdepth 1 \
                -type d \
                ! -name 00000-selected-dir \
                -print \
                -quit
        )"
        actual_selection="$(selected_path || true)"
        actual_role="$(focused_role || true)"
        if [[ -n "$created_dir"
            && "${actual_selection%/}" == "$created_dir"
            && "$actual_role" == "AXTextField" ]]; then
            break
        fi
        sleep 0.02
    done

    if (( item_count > 0 )); then
        sleep 0.5
        actual_selection="$(selected_path || true)"
        actual_role="$(focused_role || true)"
    fi

    [[ -n "$created_dir" ]] \
        || fail "$view did not create a sibling folder"
    [[ "${actual_selection%/}" == "$created_dir" ]] \
        || fail "$view did not select the created folder: $actual_selection"
    [[ "$actual_role" == "AXTextField" ]] \
        || fail "$view did not begin inline rename: $actual_role"
    [[ -z "$(
        find "$selected_dir" \
            -mindepth 1 \
            -maxdepth 1 \
            -type d \
            -print \
            -quit
    )" ]] || fail "$view created the folder inside the selected directory"

    local window_count_after
    window_count_after="$(finder_window_count)"
    [[ "$window_count_after" == "$window_count_before" ]] \
        || fail "$view opened a new Finder window: before=$window_count_before after=$window_count_after"

    close_test_windows
    sleep 0.5
    print -- "Finder $view $case_name new folder passed."
}

run_list_nested_case() {
    local fixture_dir="$test_root/list-nested"
    local parent_dir="$fixture_dir/00000-parent"
    local child_file="$parent_dir/00000-child.txt"
    mkdir -p "$parent_dir"
    touch "$child_file" "$fixture_dir/sibling.txt"

    print -- "Testing Finder list nested-child new folder..."
    open_test_window "$fixture_dir" "$parent_dir" list
    /usr/bin/osascript \
        -e 'tell application "System Events"' \
        -e 'tell process "Finder"' \
        -e 'key code 124' \
        -e 'delay 0.2' \
        -e 'key code 125' \
        -e 'end tell' \
        -e 'end tell'
    sleep 0.2

    local nested_selection
    nested_selection="$(selected_path || true)"
    [[ "${nested_selection%/}" == "$child_file" ]] \
        || fail "list nested setup did not select the child: $nested_selection"

    local window_count_before
    window_count_before="$(finder_window_count)"
    "$helper" new-folder-current-level >/dev/null

    local created_dir=""
    local actual_selection=""
    local actual_role=""
    for _ in {1..100}; do
        created_dir="$(
            find "$parent_dir" \
                -mindepth 1 \
                -maxdepth 1 \
                -type d \
                -print \
                -quit
        )"
        actual_selection="$(selected_path || true)"
        actual_role="$(focused_role || true)"
        if [[ -n "$created_dir"
            && "${actual_selection%/}" == "$created_dir"
            && "$actual_role" == "AXTextField" ]]; then
            break
        fi
        sleep 0.02
    done

    [[ -n "$created_dir" ]] \
        || fail "list nested child did not create inside its parent"
    [[ "${actual_selection%/}" == "$created_dir" ]] \
        || fail "list nested child did not select the created folder: $actual_selection"
    [[ "$actual_role" == "AXTextField" ]] \
        || fail "list nested child did not begin inline rename: $actual_role"
    [[ -z "$(
        find "$fixture_dir" \
            -mindepth 1 \
            -maxdepth 1 \
            -type d \
            ! -name 00000-parent \
            -print \
            -quit
    )" ]] || fail "list nested child created at the window root"
    [[ "$(finder_window_count)" == "$window_count_before" ]] \
        || fail "list nested child opened a new Finder window"

    close_test_windows
    sleep 0.5
    print -- "Finder list nested-child new folder passed."
}

[[ -x "$helper" ]] || fail "missing executable helper: $helper"

run_list_nested_case

for view in ${(z)${FINDER_VIM_NEW_FOLDER_LARGE_VIEWS:-list column icon}}; do
    case "$view" in
        list|column|icon) run_view_case "$view" 1000 large-scroll ;;
        *) fail "unsupported large-scroll view: $view" ;;
    esac
done

for view in ${(z)${FINDER_VIM_NEW_FOLDER_VIEWS:-list column icon}}; do
    case "$view" in
        list|column|icon) run_view_case "$view" 0 same-window ;;
        *) fail "unsupported view: $view" ;;
    esac
done

print -- "Finder same-window new-folder regressions passed."
