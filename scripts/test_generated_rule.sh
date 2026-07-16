#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
rule_file="$repo_root/rules/generated/finder-vim.json"
text_expression="accessibility.focused_ui_element.role_string like 'AXText*'"
clear_marks_command='exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt'
clear_visual_state_command='exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt $HOME/.local/state/finder-vim/finder_visual_anchor.txt'
clear_selection_command='$HOME/.local/libexec/finder-vim/finder_ax_step clear-selection >/dev/null 2>&1; exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt $HOME/.local/state/finder-vim/finder_visual_anchor.txt'
visual_start_command='$HOME/.local/libexec/finder-vim/finder_ax_move visual-start >/dev/null 2>&1; exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt'

jq -e \
    --arg text_expression "$text_expression" \
    --arg clear_marks_command "$clear_marks_command" \
    --arg clear_visual_state_command "$clear_visual_state_command" \
    --arg clear_selection_command "$clear_selection_command" \
    --arg visual_start_command "$visual_start_command" '
def finder_normal_conditions:
    ([.conditions[] | select(
        .type == "frontmost_application_if"
        and .bundle_identifiers == ["^com\\.apple\\.finder$"]
    )] | length == 1)
    and ([.conditions[] | select(
        .type == "expression_unless"
        and .expression == $text_expression
    )] | length == 1)
    and ([.conditions[] | select(
        .type == "variable_unless"
        and .name == "finder_visual_mode"
        and .value == 1
    )] | length == 1);

[
    ([
        .rules[]
        | select(.description == "Finer Utility Commands")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Press o to open the selected item"
            and .from == {"key_code":"o"}
            and .to == [{"key_code":"down_arrow","modifiers":["command"],"repeat":false}]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode -> Visual Mode: Press v to start range selection"
            and .to[0] == {"shell_command":$visual_start_command}
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            (.description == "Visual Mode: Extend selection down by Finder motion count"
                or .description == "Visual Mode: Extend selection up by Finder motion count")
            and ([.conditions[] | select(
                .name == "finder_visual_mode"
                and .type == "variable_if"
                and .value == 1
            )] | length == 1)
            and ([.to[] | select(.shell_command? != null)] | length == 99)
            and ([.to[] | select(.shell_command? != null) | .shell_command]
                | all(contains("finder_ax_move visual-")))
        )
    ] | length == 2),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            (.description == "Normal Mode: Move down by Finder motion count"
                or .description == "Normal Mode: Move up by Finder motion count")
            and ([.conditions[] | select(
                .name == "finder_visual_mode"
                and .type == "variable_unless"
                and .value == 1
            )] | length == 1)
            and ([.to[] | select(.shell_command? != null)] | length == 99)
            and ([.to[] | select(.shell_command? != null) | .shell_command]
                | all(contains("finder_ax_move visual-") | not))
        )
    ] | length == 2),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            (.description // "")
                | test("^Normal/Visual Mode: Add [0-9] to Finder motion count$")
        )
        | select([.conditions[] | select(.name == "finder_visual_mode")] | length == 0)
    ] | length == 10),
    ([
        .rules[]
        | select(.description == "Finer Utility Commands")
        | .manipulators[]
        | select(
            (.description == "Visual Mode: Press gg to extend selection to first or Grid column top"
                and .to[0].shell_command == "exec $HOME/.local/libexec/finder-vim/finder_ax_move visual-first >/dev/null 2>&1")
            or (.description == "Visual Mode: Press G to extend selection to last or Grid column bottom"
                and .to[0].shell_command == "exec $HOME/.local/libexec/finder-vim/finder_ax_move visual-last >/dev/null 2>&1")
        )
        | select([.conditions[] | select(
            .name == "finder_visual_mode"
            and .type == "variable_if"
            and .value == 1
        )] | length == 1)
    ] | length == 2),
    ([
        .rules[]
        | select(.description == "Finer Utility Commands")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Press - to open the parent directory"
            and .from == {"key_code":"hyphen"}
            and .to == [{"key_code":"up_arrow","modifiers":["command"]}]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .from == {"key_code":"h"}
            and .parameters["basic.to_if_held_down_threshold_milliseconds"] == 150
            and .to == [{"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_step hold-start left >/dev/null 2>&1"}]
            and .to_if_held_down == [{"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_step hold-repeat left >/dev/null 2>&1"}]
            and .to_after_key_up == [{"shell_command":"exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_left_hold.txt"}]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .from == {"key_code":"l"}
            and .parameters["basic.to_if_held_down_threshold_milliseconds"] == 150
            and .to == [{"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_step hold-start right >/dev/null 2>&1"}]
            and .to_if_held_down == [{"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_step hold-repeat right >/dev/null 2>&1"}]
            and .to_after_key_up == [{"shell_command":"exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_right_hold.txt"}]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Press Esc to clear discontiguous Finder marks"
            and .from == {"key_code":"escape"}
            and .to[0] == {"shell_command":$clear_selection_command}
            and ([.to[] | select(.shell_command? != null)] | length == 1)
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Toggle discontiguous Finder mark with s"
            and .from == {"key_code":"s"}
            and .to == [{"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_step toggle-mark >/dev/null 2>&1"}]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[].manipulators[].to[]?
        | select(.key_code == "a")
        | select((.modifiers // []) | index("command"))
        | select((.modifiers // []) | index("option"))
    ] | length == 0),
    ([
        .rules[].manipulators[].to[]?.shell_command?
        | select(type == "string")
        | select(contains("/finder_marks.txt"))
    ] | length > 0),
    ([
        .rules[].manipulators[].to[]?.shell_command?
        | select(type == "string")
        | select(contains("/finder_marks.txt"))
        | (. == $clear_marks_command
            or . == $clear_visual_state_command
            or . == $clear_selection_command
            or . == $visual_start_command)
    ] | all)
] | all
' "$rule_file" >/dev/null

print -- "Generated rule regression tests passed."
