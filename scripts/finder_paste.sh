#!/bin/zsh

mode="$1"
state_file="${KARABINER_FINDER_MARKS_FILE:-$HOME/.local/state/finder-vim/finder_marks.txt}"
copy_file="${KARABINER_FINDER_COPY_FILE:-$HOME/.local/state/finder-vim/finder_copy.txt}"
cut_file="${KARABINER_FINDER_CUT_FILE:-$HOME/.local/state/finder-vim/finder_cut.txt}"
mkdir -p "$(dirname "$state_file")"
touch "$copy_file" "$cut_file"

case "$mode" in
    copy)
        source_file="$copy_file"
        finder_action="duplicate"
        ;;
    move)
        source_file="$cut_file"
        finder_action="move"
        ;;
    *)
        exit 2
        ;;
esac

paths=()
while IFS= read -r line; do
    [ -n "$line" ] && paths+=("$line")
done < "$source_file"

if [ "${#paths[@]}" -eq 0 ]; then
    exit 0
fi

if ! osascript - "$finder_action" "${paths[@]}" <<'APPLESCRIPT'; then
on run argv
    set finderAction to item 1 of argv
    set sourceItems to {}

    repeat with i from 2 to count of argv
        try
            set end of sourceItems to (POSIX file (item i of argv) as alias)
        end try
    end repeat

    if (count of sourceItems) is 0 then return

    tell application "Finder"
        activate
        try
            set destinationFolder to target of front window
        on error
            set destinationFolder to desktop
        end try

        repeat with sourceItem in sourceItems
            if finderAction is "duplicate" then
                duplicate (contents of sourceItem) to destinationFolder
            else
                move (contents of sourceItem) to destinationFolder
            end if
        end repeat
    end tell
end run
APPLESCRIPT
    exit 1
fi

if [ "$mode" = "move" ]; then
    : > "$state_file"
    : > "$copy_file"
    : > "$cut_file"
fi
