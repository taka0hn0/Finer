#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
fixture_source="$fixture_root/items-1000/01-A"
helper="${FINDER_VIM_HELPER:-$repo_root/.build/finder_ax_step}"
test_root="$(mktemp -d "$repo_root/.build/finder-edge-monitor-test.XXXXXX")"
fixture_dir="$test_root/items"
marks_file="$test_root/finder_marks.txt"
anchor_file="$test_root/finder_navigation_anchor.txt"
window_id=""

cp -R "$fixture_source" "$fixture_dir"
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
    window_id=""
    for _ in {1..5}; do
        if window_id="$(
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
                -e 'end run' -- "$fixture_dir" "$view" 2>/dev/null
        )"; then
            break
        fi
        window_id=""
        sleep 0.1
    done
    [[ -n "$window_id" ]] || fail "could not open the $view test window"
    sleep 1
    activate_test_window
}

run_helper() {
    KARABINER_FINDER_MARKS_FILE="$marks_file" \
    KARABINER_FINDER_ANCHOR_FILE="$anchor_file" \
        "$helper" "$@"
}

selected_path() {
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

require_scroll_edge() {
    local view="$1"
    local edge="$2"
    local value=""
    for _ in {1..100}; do
        value="$(run_helper vertical-scroll-value 2>/dev/null || true)"
        if [[ "$value" =~ '^[0-9]+([.][0-9]+)?$' ]]; then
            if [[ "$edge" == "top" ]] && (( value <= 0.25 )); then
                return
            fi
            if [[ "$edge" == "bottom" ]] && (( value >= 0.75 )); then
                return
            fi
        fi
        sleep 0.02
    done
    fail "$view viewport did not reach the $edge range: scroll=$value"
}

require_selected_visible() {
    local label="$1"
    for _ in {1..100}; do
        if [[ "$(run_helper selected-visible 2>/dev/null || true)" == "1" ]]; then
            return
        fi
        sleep 0.02
    done
    fail "$label selected item is outside the actual visible List rows"
}

require_scroll_inside_edge() {
    local label="$1"
    local direction="$2"
    local value=""
    for _ in {1..100}; do
        value="$(run_helper vertical-scroll-value 2>/dev/null || true)"
        if [[ "$value" =~ '^[0-9]+([.][0-9]+)?$' ]]; then
            if [[ "$direction" == "down" ]] \
                    && (( value > 0.01 && value < 0.75 )); then
                return
            fi
            if [[ "$direction" == "up" ]] \
                    && (( value < 0.99 && value > 0.25 )); then
                return
            fi
        fi
        sleep 0.02
    done
    fail "$label viewport did not leave the starting edge: scroll=$value"
}

run_view_case() {
    local view="$1"
    print -- "Testing Finder $view default edge wrap action..."
    open_test_window "$view"

    if [[ "$view" == "list" ]]; then
        run_helper first >/dev/null
        wait_for_path "$fixture_dir/item-00000.txt" \
            || fail "list count-move setup did not reach item-00000"
        local count_start_state
        count_start_state="$(run_helper scroll-state)"
        local count_position
        count_position="$(run_helper count-move down 5)"
        wait_for_path "$fixture_dir/item-00005.txt" \
            || fail "list count-move down did not reach item-00005: start=[$count_start_state] result=$count_position"
        require_selected_visible "list count-move down"
        run_helper count-move up 5 >/dev/null
        wait_for_path "$fixture_dir/item-00000.txt" \
            || fail "list count-move up did not return to item-00000"
        require_selected_visible "list count-move up"

        run_helper hold-start down >/dev/null
        (
            sleep 0.8
            : > "$HOME/.local/state/finder-vim/finder_down_hold.txt"
        ) &
        local normal_stopper_pid=$!
        run_helper hold-repeat down >/dev/null
        wait "$normal_stopper_pid"
        require_selected_visible "list downward non-wrapping hold"
        require_scroll_inside_edge "list downward non-wrapping hold" down

        run_helper last >/dev/null
        run_helper hold-start up >/dev/null
        (
            sleep 0.8
            : > "$HOME/.local/state/finder-vim/finder_up_hold.txt"
        ) &
        normal_stopper_pid=$!
        run_helper hold-repeat up >/dev/null
        wait "$normal_stopper_pid"
        require_selected_visible "list upward non-wrapping hold"
        require_scroll_inside_edge "list upward non-wrapping hold" up

        run_helper first >/dev/null
        run_helper hold-start down >/dev/null
        run_helper hold-repeat down >/dev/null &
        local reversing_down_pid=$!
        sleep 0.45
        run_helper hold-start up >/dev/null
        wait "$reversing_down_pid"
        (
            sleep 0.12
            : > "$HOME/.local/state/finder-vim/finder_up_hold.txt"
        ) &
        local reversing_up_stopper_pid=$!
        run_helper hold-repeat up >/dev/null
        wait "$reversing_up_stopper_pid"
        local reversed_path
        reversed_path="$(selected_path)"
        [[ "$reversed_path" == "$fixture_dir/item-000"[0-9][0-9]".txt" ]] \
            || fail "list down-to-up reversal jumped outside the leading range: $reversed_path"
        require_selected_visible "list down-to-up reversal"
    fi

    run_helper last >/dev/null
    wait_for_path "$fixture_dir/item-00999.txt" \
        || fail "$view did not reach the last item"
    local wrapped_position
    wrapped_position="$(run_helper down-wrap)"
    [[ "$wrapped_position" =~ '^[1-9][0-9]*$' ]] \
        || fail "$view downward wrap action failed: $wrapped_position"
    wait_for_path "$fixture_dir/item-00000.txt" \
        || fail "$view did not wrap to the first item"
    require_scroll_edge "$view downward wrap" top
    if [[ "$view" == "list" ]]; then
        require_selected_visible "list downward wrap"
    fi

    wrapped_position="$(run_helper up-wrap)"
    [[ "$wrapped_position" =~ '^[1-9][0-9]*$' ]] \
        || fail "$view upward wrap action failed: $wrapped_position"
    wait_for_path "$fixture_dir/item-00999.txt" \
        || fail "$view did not wrap to the last item"
    require_scroll_edge "$view upward wrap" bottom
    if [[ "$view" == "list" ]]; then
        require_selected_visible "list upward wrap"
    fi

    if [[ "$view" == "list" || "$view" == "column" ]]; then
        for _ in {1..10}; do
            run_helper up-wrap >/dev/null
        done
        wait_for_path "$fixture_dir/item-00989.txt" \
            || fail "list held-wrap setup did not reach item-00989"
        run_helper hold-start down >/dev/null
        sleep 0.1
        (
            sleep 0.8
            : > "$HOME/.local/state/finder-vim/finder_down_hold.txt"
        ) &
        local stopper_pid=$!
        local repeat_position
        repeat_position="$(run_helper hold-repeat down)"
        wait "$stopper_pid"
        [[ "$repeat_position" =~ '^[1-9][0-9]*$' ]] \
            || fail "list held wrap action failed: $repeat_position"
        local held_path
        held_path="$(selected_path)"
        [[ "$held_path" == "$fixture_dir/item-00"[0-1][0-9][0-9]".txt" ]] \
            || fail "list held wrap did not cross to the leading range: $held_path"
        require_scroll_edge "$view downward held wrap" top
        if [[ "$view" == "list" ]]; then
            require_selected_visible "list downward held wrap"
        fi

        run_helper first >/dev/null
        for _ in {1..10}; do
            run_helper down-wrap >/dev/null
        done
        wait_for_path "$fixture_dir/item-00010.txt" \
            || fail "list upward held-wrap setup did not reach item-00010"
        run_helper hold-start up >/dev/null
        sleep 0.1
        (
            sleep 0.8
            : > "$HOME/.local/state/finder-vim/finder_up_hold.txt"
        ) &
        stopper_pid=$!
        repeat_position="$(run_helper hold-repeat up)"
        wait "$stopper_pid"
        [[ "$repeat_position" =~ '^[1-9][0-9]*$' ]] \
            || fail "list upward held wrap action failed: $repeat_position"
        held_path="$(selected_path)"
        [[ "$held_path" == "$fixture_dir/item-00"[8-9][0-9][0-9]".txt" ]] \
            || fail "list upward held wrap did not cross to the trailing range: $held_path"
        require_scroll_edge "$view upward held wrap" bottom
        if [[ "$view" == "list" ]]; then
            require_selected_visible "list upward held wrap"
        fi
    fi

    close_test_window
    print -- "Finder $view default edge wrap action passed."
}

[[ -x "$helper" ]] || fail "missing executable helper: $helper"
[[ -f "$fixture_dir/item-00000.txt"
    && -f "$fixture_dir/item-00999.txt" ]] \
    || fail "missing 1000-item fixture: $fixture_dir"

test_views="${FINDER_VIM_TEST_VIEWS:-list column}"
for view in ${=test_views}; do
    run_view_case "$view"
done

print -- "Finder default edge wrap regressions passed."
