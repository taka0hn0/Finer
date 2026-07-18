#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
helper="${FINDER_VIM_HELPER:-$repo_root/.build/finder_ax_step}"
action_helper="${FINDER_VIM_ACTION_HELPER:-$repo_root/scripts/finder_action_marked.sh}"
test_root="$(mktemp -d "$repo_root/.build/finder-selection-test.XXXXXX")"
fixture_dir="$test_root/fixture"
state_dir="$test_root/state"
marks_file="$state_dir/finder_marks.txt"
anchor_file="$state_dir/finder_navigation_anchor.txt"
copy_file="$state_dir/finder_copy.txt"
cut_file="$state_dir/finder_cut.txt"
window_id=""

mkdir -p "$fixture_dir" "$state_dir"
touch \
    "$fixture_dir/00-A.txt" \
    "$fixture_dir/01-B.txt" \
    "$fixture_dir/02-C.txt" \
    "$fixture_dir/03-D.txt"
: > "$marks_file"
: > "$anchor_file"
: > "$copy_file"
: > "$cut_file"

fail() {
    print -u2 -- "Finder selection regression: $1"
    exit 1
}

close_test_window() {
    if [[ -n "$window_id" ]]; then
        /usr/bin/osascript \
            -e 'on run argv' \
            -e 'set windowId to (item 1 of argv) as integer' \
            -e 'tell application "Finder"' \
            -e 'if exists (first Finder window whose id is windowId) then close (first Finder window whose id is windowId)' \
            -e 'end tell' \
            -e 'end run' -- "$window_id" >/dev/null 2>&1 || true
        window_id=""
    fi
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
                -e 'set testWindow to make new Finder window to targetFolder' \
                -e 'if viewName is "list" then' \
                -e 'set current view of testWindow to list view' \
                -e 'set sort column of list view options of testWindow to name column' \
                -e 'else if viewName is "column" then' \
                -e 'set current view of testWindow to column view' \
                -e 'else' \
                -e 'set current view of testWindow to icon view' \
                -e 'set arrangement of icon view options of testWindow to arranged by name' \
                -e 'end if' \
                -e 'set selection to {POSIX file (fixturePath & "/00-A.txt") as alias}' \
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

selected_names() {
    activate_test_window
    /usr/bin/osascript \
        -e 'tell application "Finder"' \
        -e 'set outputText to ""' \
        -e 'repeat with selectedItem in (get selection)' \
        -e 'set outputText to outputText & name of selectedItem & linefeed' \
        -e 'end repeat' \
        -e 'return outputText' \
        -e 'end tell' 2>/dev/null \
        | sed '/^$/d' \
        | sort \
        | paste -sd, -
}

wait_for_selection() {
    local expected="$1"
    local actual=""
    for _ in {1..100}; do
        actual="$(selected_names)"
        [[ "$actual" == "$expected" ]] && return 0
        sleep 0.02
    done
    print -u2 -- "expected selection=$expected actual=$actual"
    return 1
}

wait_for_fixture_items_cleared() {
    local actual=""
    for _ in {1..100}; do
        actual="$(selected_names)"
        if [[ "$actual" != *00-A.txt*
            && "$actual" != *01-B.txt*
            && "$actual" != *02-C.txt*
            && "$actual" != *03-D.txt* ]]; then
            return 0
        fi
        sleep 0.02
    done
    print -u2 -- "fixture selection remained after clear: $actual"
    return 1
}

wait_for_marks() {
    local expected="$1"
    local actual=""
    for _ in {1..100}; do
        actual="$(sed '/^$/d' "$marks_file" | sort | xargs -n1 basename | paste -sd, -)"
        [[ "$actual" == "$expected" ]] && return 0
        sleep 0.02
    done
    print -u2 -- "expected marks=$expected actual=$actual"
    return 1
}

reset_state() {
    run_helper clear-selection >/dev/null
    sleep 0.2
    : > "$marks_file"
    : > "$anchor_file"
    : > "$copy_file"
    : > "$cut_file"
}

run_view_case() {
    local view="$1"
    local direction="$2"
    print -- "Testing Finder $view selection..."
    open_test_window "$view"

    run_helper toggle-mark
    wait_for_marks "00-A.txt" || fail "$view did not mark A"
    wait_for_selection "00-A.txt" || fail "$view did not display marked A"

    run_helper hold-start "$direction"
    wait_for_selection "00-A.txt,01-B.txt" \
        || fail "$view did not preserve A while moving to B"

    run_helper toggle-mark
    wait_for_marks "00-A.txt,01-B.txt" || fail "$view did not mark B"

    run_helper hold-start "$direction"
    wait_for_selection "00-A.txt,01-B.txt,02-C.txt" \
        || fail "$view did not preserve confirmed marks at C"
    wait_for_marks "00-A.txt,01-B.txt" \
        || fail "$view promoted transient C to a confirmed mark"

    if [[ "$view" == list ]]; then
        KARABINER_FINDER_MARKS_FILE="$marks_file" \
        KARABINER_FINDER_ANCHOR_FILE="$anchor_file" \
        KARABINER_FINDER_COPY_FILE="$copy_file" \
        KARABINER_FINDER_CUT_FILE="$cut_file" \
            "$action_helper" copy
        local copied
        copied="$(sed '/^$/d' "$copy_file" | sort | xargs -n1 basename | paste -sd, -)"
        [[ "$copied" == "00-A.txt,01-B.txt" ]] \
            || fail "copy included transient cursor: $copied"

        run_helper hold-start down
        sleep 0.1
        (
            sleep 0.2
            truncate -s 0 "$HOME/.local/state/finder-vim/finder_down_hold.txt"
        ) &
        local stopper_pid=$!
        run_helper hold-repeat down >/dev/null
        wait "$stopper_pid"
        wait_for_marks "00-A.txt,01-B.txt" \
            || fail "marked hold changed the confirmed marks"
        local held_selection
        held_selection="$(selected_names)"
        [[ "$held_selection" == *00-A.txt*
            && "$held_selection" == *01-B.txt* ]] \
            || fail "marked hold lost a confirmed selection: $held_selection"
    fi

    reset_state
    if [[ "$view" == column ]]; then
        wait_for_fixture_items_cleared \
            || fail "$view Esc path did not clear the active column selection"
    else
        wait_for_selection "" || fail "$view Esc path did not clear selection"
    fi
    close_test_window
    sleep 1
    print -- "Finder $view selection passed."
}

[[ -x "$helper" ]] || fail "missing executable helper: $helper"

for view in ${(z)${FINDER_VIM_SELECTION_VIEWS:-list column icon}}; do
    case "$view" in
        list|column) run_view_case "$view" down ;;
        icon) run_view_case "$view" right ;;
        *) fail "unsupported view: $view" ;;
    esac
done

print -- "Finder selection regressions passed."
