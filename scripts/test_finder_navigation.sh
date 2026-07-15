#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
fixture_root="${FINDER_VIM_FIXTURE_ROOT:-$repo_root/.build/benchmark-fixtures/column-race}"
helper="${FINDER_VIM_HELPER:-$HOME/.local/libexec/finder-vim/finder_ax_step}"

if [[ ! -x "$helper" ]]; then
    print -u2 -- "Missing executable helper: $helper"
    print -u2 -- "Run make install or set FINDER_VIM_HELPER."
    exit 1
fi

group_shortcut() {
    case "$1" in
        Name) print 1 ;;
        Kind) print 2 ;;
        DateLastOpened) print 3 ;;
        DateAdded) print 4 ;;
        DateModified) print 5 ;;
        Size) print 6 ;;
        Tags) print 7 ;;
        *) return 1 ;;
    esac
}

original_group_arrangement="$(defaults read com.apple.finder FXArrangeGroupViewBy 2>/dev/null || print Name)"
original_group_preference="$(defaults read com.apple.finder FXPreferredGroupBy 2>/dev/null || print None)"
original_group_shortcut="$(group_shortcut "$original_group_arrangement" || true)"
if [[ -z "$original_group_shortcut" ]]; then
    print -u2 -- "Unsupported Finder grouping preference: $original_group_arrangement"
    exit 1
fi

window_id=""
grouping_changed=false

activate_test_window() {
    typeset -a activate_script=(
        -e 'set windowId to (system attribute "FINDER_VIM_WINDOW_ID") as integer'
        -e 'tell application "Finder"'
        -e 'set testWindow to first Finder window whose id is windowId'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'end tell'
    )
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript "${activate_script[@]}" \
        >/dev/null 2>&1
}

send_group_shortcut() {
    local key="$1"
    activate_test_window
    FINDER_VIM_GROUP_KEY="$key" /usr/bin/osascript \
        -e 'set groupKey to system attribute "FINDER_VIM_GROUP_KEY"' \
        -e 'tell application "System Events" to keystroke groupKey using {control down, command down}' \
        >/dev/null
    sleep 0.2
}

restore_grouping() {
    if [[ "$grouping_changed" != true || -z "$window_id" ]]; then
        return
    fi
    send_group_shortcut "$original_group_shortcut" || true
    if [[ "$original_group_preference" == None ]]; then
        send_group_shortcut 0 || true
    fi
    grouping_changed=false
}

close_test_window() {
    if [[ -z "$window_id" ]]; then
        return
    fi
    restore_grouping
    typeset -a close_script=(
        -e 'set windowId to (system attribute "FINDER_VIM_WINDOW_ID") as integer'
        -e 'tell application "Finder"'
        -e 'if exists (first Finder window whose id is windowId) then close (first Finder window whose id is windowId)'
        -e 'end tell'
    )
    FINDER_VIM_WINDOW_ID="$window_id" /usr/bin/osascript "${close_script[@]}" \
        >/dev/null 2>&1 || true
    window_id=""
    sleep 1
}

open_test_window() {
    local view="$1"
    local case_dir="$2"
    local initial_name="$3"
    typeset -a open_script=(
        -e 'set casePath to system attribute "FINDER_VIM_CASE_DIR"'
        -e 'set initialName to system attribute "FINDER_VIM_INITIAL_NAME"'
        -e 'set viewName to system attribute "FINDER_VIM_VIEW"'
        -e 'tell application "Finder"'
        -e 'set targetFolder to POSIX file casePath as alias'
        -e 'set testWindow to make new Finder window to targetFolder'
        -e 'if viewName is "list" then'
        -e 'set current view of testWindow to list view'
        -e 'set sort column of list view options of testWindow to name column'
        -e 'else'
        -e 'set current view of testWindow to icon view'
        -e 'set arrangement of icon view options of testWindow to arranged by name'
        -e 'end if'
        -e 'set selection to {file initialName of targetFolder}'
        -e 'set index of testWindow to 1'
        -e 'activate'
        -e 'return id of testWindow'
        -e 'end tell'
    )
    window_id="$(
        FINDER_VIM_CASE_DIR="$case_dir" \
        FINDER_VIM_INITIAL_NAME="$initial_name" \
        FINDER_VIM_VIEW="$view" \
            /usr/bin/osascript "${open_script[@]}"
    )"
    sleep 1
    activate_test_window
    "$helper" first >/dev/null
    sleep 0.2
}

selected_path() {
    activate_test_window
    /usr/bin/osascript \
        -e 'tell application "Finder" to POSIX path of (item 1 of (get selection) as alias)'
}

send_step() {
    "$helper" hold-start "$1"
    sleep 1
}

trap close_test_window EXIT
trap 'close_test_window; exit 130' INT
trap 'close_test_window; exit 143' TERM

mixed_dir="$fixture_root/items-10"
if [[ ! -f "$mixed_dir/00-start.txt"
    || ! -d "$mixed_dir/01-A"
    || ! -f "$mixed_dir/02-sibling.txt" ]]; then
    print -u2 -- "Missing mixed fixture: $mixed_dir"
    exit 1
fi

open_test_window list "$mixed_dir" 00-start.txt
grouping_changed=true
send_group_shortcut 2
"$helper" first >/dev/null
sleep 0.2
grouped_first="$(selected_path)"
send_step up
grouped_last="$(selected_path)"
send_step down
grouped_wrapped="$(selected_path)"
if [[ "$grouped_first" == "$grouped_last"
    || "$grouped_wrapped" != "$grouped_first" ]]; then
    print -u2 -- "Grouped mixed List View regression failed"
    exit 1
fi
close_test_window

list_dir="$fixture_root/items-10/01-A"
open_test_window list "$list_dir" item-00000.txt
"$helper" hold-start down
sleep 0.1
(
    sleep 1
    truncate -s 0 "$HOME/.local/state/finder-vim/finder_down_hold.txt"
) &
stopper_pid=$!
repeat_result="$("$helper" hold-repeat down)"
wait "$stopper_pid"
held_path="$(selected_path)"
if [[ ! "$repeat_result" =~ '^[1-9][0-9]*$'
    || "$held_path" != "$list_dir"/item-*.txt ]]; then
    print -u2 -- "Held List View regression failed: result=$repeat_result path=$held_path"
    exit 1
fi
close_test_window

open_test_window icon "$list_dir" item-00000.txt
for ((step = 0; step < 10; ++step)); do
    "$helper" hold-start right
done
sleep 2
icon_forward="$(selected_path)"
send_step left
icon_reverse="$(selected_path)"
if [[ "$icon_forward" != "$list_dir/item-00000.txt"
    || "$icon_reverse" != "$list_dir/item-00009.txt" ]]; then
    print -u2 -- "Icon View wrap regression failed"
    exit 1
fi
close_test_window

print -- "Finder navigation regressions passed."
