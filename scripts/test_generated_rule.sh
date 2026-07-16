#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
rule_file="$repo_root/rules/generated/finder-vim.json"
text_expression="accessibility.focused_ui_element.role_string like 'AXText*'"

jq -e --arg text_expression "$text_expression" '
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
    ] | length == 1)
] | all
' "$rule_file" >/dev/null

print -- "Generated rule regression tests passed."
