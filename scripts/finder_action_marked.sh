#!/bin/zsh

action="$1"
state_file="${KARABINER_FINDER_MARKS_FILE:-$HOME/.local/state/finder-vim/finder_marks.txt}"
copy_file="${KARABINER_FINDER_COPY_FILE:-$HOME/.local/state/finder-vim/finder_copy.txt}"
cut_file="${KARABINER_FINDER_CUT_FILE:-$HOME/.local/state/finder-vim/finder_cut.txt}"
mkdir -p "$(dirname "$state_file")"
touch "$state_file" "$copy_file" "$cut_file"

case "$action" in
    copy|delete|cut|copy-current|delete-current|cut-current)
        ;;
    *)
        exit 2
        ;;
esac

read_marked_paths() {
    local line
    while IFS= read -r line; do
        [ -n "$line" ] && paths+=("$line")
    done < "$state_file"
}

read_selection_paths() {
    local selection_text line
    selection_text="$(osascript <<'APPLESCRIPT'
tell application "Finder"
    set selectedItems to selection
    set outputText to ""
    repeat with selectedItem in selectedItems
        try
            set outputText to outputText & POSIX path of (selectedItem as alias) & linefeed
        end try
    end repeat
    return outputText
end tell
APPLESCRIPT
)"

    while IFS= read -r line; do
        [ -n "$line" ] && paths+=("$line")
    done <<< "$selection_text"
}

write_paths() {
    local destination="$1"
    : > "$destination"
    local path
    for path in "${paths[@]}"; do
        print -r -- "$path" >> "$destination"
    done
}

paths=()
case "$action" in
    *-current)
        read_selection_paths
        ;;
    *)
        read_marked_paths
        if [ "${#paths[@]}" -eq 0 ]; then
            read_selection_paths
        fi
        ;;
esac

case "$action" in
    copy|copy-current)
        write_paths "$copy_file"
        : > "$cut_file"
        ;;
    cut|cut-current)
        write_paths "$cut_file"
        : > "$copy_file"
        : > "$state_file"
        ;;
    delete|delete-current)
        if [ "${#paths[@]}" -eq 0 ]; then
            exit 0
        fi

        osascript - "${paths[@]}" <<'APPLESCRIPT'
on run argv
    set deleteItems to {}
    repeat with itemPath in argv
        try
            set end of deleteItems to (POSIX file (contents of itemPath) as alias)
        end try
    end repeat

    if (count of deleteItems) is 0 then return

    tell application "Finder"
        delete deleteItems
    end tell
end run
APPLESCRIPT
        : > "$state_file"
        ;;
esac
