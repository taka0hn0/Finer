#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
rule_file="$repo_root/rules/generated/finder-vim.json"
text_expression="accessibility.focused_ui_element.role_string like 'AXText*'"
list_role_expression="accessibility.focused_ui_element.role_string == 'AXOutline'"
clear_marks_command='exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt'
clear_visual_state_command='exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt $HOME/.local/state/finder-vim/finder_visual_anchor.txt'
clear_selection_command='$HOME/.local/libexec/finder-vim/finder_ax_step clear-selection >/dev/null 2>&1; exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt $HOME/.local/state/finder-vim/finder_visual_anchor.txt'
visual_start_command='$HOME/.local/libexec/finder-vim/finder_ax_move visual-start >/dev/null 2>&1; exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt'
copy_marks_command='$HOME/.local/libexec/finder-vim/finder_action_marked.sh copy >/dev/null 2>&1; exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_marks.txt $HOME/.local/state/finder-vim/finder_navigation_anchor.txt'

jq -e \
    --arg text_expression "$text_expression" \
    --arg list_role_expression "$list_role_expression" \
    --arg clear_marks_command "$clear_marks_command" \
    --arg clear_visual_state_command "$clear_visual_state_command" \
    --arg clear_selection_command "$clear_selection_command" \
    --arg visual_start_command "$visual_start_command" \
    --arg copy_marks_command "$copy_marks_command" '
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

def finder_visual_conditions:
    ([.conditions[] | select(
        .type == "frontmost_application_if"
        and .bundle_identifiers == ["^com\\.apple\\.finder$"]
    )] | length == 1)
    and ([.conditions[] | select(
        .type == "expression_unless"
        and .expression == $text_expression
    )] | length == 1)
    and ([.conditions[] | select(
        .type == "variable_if"
        and .name == "finder_visual_mode"
        and .value == 1
    )] | length == 1);

def exact_visual_count_commands($direction):
    ([.to[] | select(.shell_command? != null) | {
        shell_command,
        conditions
    }]) == ([range(1; 100) as $count | {
        shell_command: ("exec $HOME/.local/libexec/finder-vim/finder_ax_move visual-\($direction) \($count) >/dev/null 2>&1"),
        conditions: [{
            expression: ("finder_motion_count == \($count)"),
            type: "expression_if"
        }]
    }]);

def clears_motion_count:
    .to[-2:] == [
        {"set_variable":{"name":"finder_motion_count","value":0}},
        {"set_variable":{"name":"finder_motion_count_expiration","value":0}}
    ];

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
            .description == "Experimental List native hold: Map j directly to Down Arrow"
            and .from == {"key_code":"j"}
            and .to == [{"key_code":"down_arrow","repeat":true}]
        )
        | select(
            .conditions == [
                {
                    "bundle_identifiers":["^com\\.apple\\.finder$"],
                    "type":"frontmost_application_if"
                },
                {"expression":$text_expression,"type":"expression_unless"},
                {"name":"finder_visual_mode","type":"variable_unless","value":1},
                {"name":"finder_confirmed_marks_maybe_present","type":"variable_unless","value":1},
                {
                    "expression":$list_role_expression,
                    "type":"expression_if"
                },
                {
                    "name":"finder_native_list_hold_experiment",
                    "type":"variable_if",
                    "value":1
                },
                {
                    "name":"finder_native_list_edge_wrap_experiment",
                    "type":"variable_unless",
                    "value":1
                }
            ]
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Experimental List native hold: Map k directly to Up Arrow"
            and .from == {"key_code":"k"}
            and .to == [{"key_code":"up_arrow","repeat":true}]
        )
        | select(
            .conditions == [
                {
                    "bundle_identifiers":["^com\\.apple\\.finder$"],
                    "type":"frontmost_application_if"
                },
                {"expression":$text_expression,"type":"expression_unless"},
                {"name":"finder_visual_mode","type":"variable_unless","value":1},
                {"name":"finder_confirmed_marks_maybe_present","type":"variable_unless","value":1},
                {
                    "expression":$list_role_expression,
                    "type":"expression_if"
                },
                {
                    "name":"finder_native_list_hold_experiment",
                    "type":"variable_if",
                    "value":1
                },
                {
                    "name":"finder_native_list_edge_wrap_experiment",
                    "type":"variable_unless",
                    "value":1
                }
            ]
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Experimental List native hold with delayed edge monitor: Map j to Down Arrow"
            and .from == {"key_code":"j"}
            and .parameters == {"basic.to_delayed_action_delay_milliseconds":250}
            and .to == [
                {"set_variable":{"name":"finder_native_list_j_pressed","value":1}},
                {"key_code":"down_arrow","repeat":true}
            ]
            and .to_after_key_up == [
                {"set_variable":{"name":"finder_native_list_j_pressed","value":0}}
            ]
            and .to_delayed_action == {
                "to_if_invoked":[{
                    "conditions":[{
                        "name":"finder_native_list_j_pressed",
                        "type":"variable_if",
                        "value":1
                    }],
                    "shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_step list-edge-monitor-start down >/dev/null 2>&1"
                }]
            }
        )
        | select(
            .conditions == [
                {
                    "bundle_identifiers":["^com\\.apple\\.finder$"],
                    "type":"frontmost_application_if"
                },
                {"expression":$text_expression,"type":"expression_unless"},
                {"name":"finder_visual_mode","type":"variable_unless","value":1},
                {"name":"finder_confirmed_marks_maybe_present","type":"variable_unless","value":1},
                {"expression":$list_role_expression,"type":"expression_if"},
                {"name":"finder_native_list_hold_experiment","type":"variable_if","value":1},
                {"name":"finder_native_list_edge_wrap_experiment","type":"variable_if","value":1}
            ]
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Experimental List native hold with delayed edge monitor: Map k to Up Arrow"
            and .from == {"key_code":"k"}
            and .parameters == {"basic.to_delayed_action_delay_milliseconds":250}
            and .to == [
                {"set_variable":{"name":"finder_native_list_k_pressed","value":1}},
                {"key_code":"up_arrow","repeat":true}
            ]
            and .to_after_key_up == [
                {"set_variable":{"name":"finder_native_list_k_pressed","value":0}}
            ]
            and .to_delayed_action == {
                "to_if_invoked":[{
                    "conditions":[{
                        "name":"finder_native_list_k_pressed",
                        "type":"variable_if",
                        "value":1
                    }],
                    "shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_step list-edge-monitor-start up >/dev/null 2>&1"
                }]
            }
        )
        | select(
            .conditions == [
                {
                    "bundle_identifiers":["^com\\.apple\\.finder$"],
                    "type":"frontmost_application_if"
                },
                {"expression":$text_expression,"type":"expression_unless"},
                {"name":"finder_visual_mode","type":"variable_unless","value":1},
                {"name":"finder_confirmed_marks_maybe_present","type":"variable_unless","value":1},
                {"expression":$list_role_expression,"type":"expression_if"},
                {"name":"finder_native_list_hold_experiment","type":"variable_if","value":1},
                {"name":"finder_native_list_edge_wrap_experiment","type":"variable_if","value":1}
            ]
        )
    ] | length == 1),
    (
        [.rules[]
            | select(.description == "Finer Navigation")
            | .manipulators
            | to_entries[]
            | select(.value.description == "Experimental List native hold: Map j directly to Down Arrow")
            | .key][0]
        < [.rules[]
            | select(.description == "Finer Navigation")
            | .manipulators
            | to_entries[]
            | select(.value.description == "Normal Mode: Map j to List wrap or Grid down with transient C worker")
            | .key][0]
    ),
    (
        [.rules[]
            | select(.description == "Finer Navigation")
            | .manipulators
            | to_entries[]
            | select(.value.description == "Experimental List native hold: Map k directly to Up Arrow")
            | .key][0]
        < [.rules[]
            | select(.description == "Finer Navigation")
            | .manipulators
            | to_entries[]
            | select(.value.description == "Normal Mode: Map k to List wrap or Grid up with transient C worker")
            | .key][0]
    ),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode -> Visual Mode: Press v to start range selection"
            and .from == {"key_code":"v"}
            and .to == [
                {"shell_command":$visual_start_command},
                {"set_variable":{"name":"finder_confirmed_marks_maybe_present","value":0}},
                {"set_variable":{"name":"finder_visual_mode","value":1}}
            ]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Visual Mode: Extend selection down by Finder motion count"
            and .from == {"key_code":"j"}
            and finder_visual_conditions
            and exact_visual_count_commands("down")
            and clears_motion_count
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Visual Mode: Extend selection up by Finder motion count"
            and .from == {"key_code":"k"}
            and finder_visual_conditions
            and exact_visual_count_commands("up")
            and clears_motion_count
        )
    ] | length == 1),
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
                and .from == {"key_code":"g"}
                and .to == [
                    {"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_move visual-first >/dev/null 2>&1"},
                    {"set_variable":{"name":"finder_g_prefix","value":0}}
                ]
                and ([.conditions[] | select(
                    .name == "finder_g_prefix"
                    and .type == "variable_if"
                    and .value == 1
                )] | length == 1))
            or (.description == "Visual Mode: Press G to extend selection to last or Grid column bottom"
                and .from == {
                    "key_code":"g",
                    "modifiers":{"mandatory":["shift"],"optional":["caps_lock"]}
                }
                and .to == [
                    {"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_move visual-last >/dev/null 2>&1"}
                ])
        )
        | select(finder_visual_conditions)
    ] | length == 2),
    ([
        .rules[]
        | select(.description == "Finer Utility Commands")
        | .manipulators[]
        | select(
            .description == "Normal/Visual Mode: Start gg sequence"
            and .from == {"key_code":"g"}
            and .to == [{"set_variable":{"name":"finder_g_prefix","value":1}}]
            and ([.conditions[] | select(.name == "finder_visual_mode")] | length == 0)
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Visual Mode: Press Esc to leave Visual Mode and clear selection"
            and .from == {"key_code":"escape"}
            and .to == [
                {"set_variable":{"name":"finder_visual_mode","value":0}},
                {"set_variable":{"name":"finder_motion_count","value":0}},
                {"set_variable":{"name":"finder_motion_count_expiration","value":0}},
                {"shell_command":$clear_selection_command},
                {"set_variable":{"name":"finder_confirmed_marks_maybe_present","value":0}},
                {"set_variable":{"name":"finder_cut_pending","value":0}},
                {"set_variable":{"name":"finder_copy_pending","value":0}}
            ]
            and finder_visual_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            (.description == "Visual Mode: Copy selection with y and return to Normal Mode"
                or .description == "Visual Mode: Move selection to Trash with d and return to Normal Mode")
            and ([.to[] | select(.shell_command? == $clear_visual_state_command)] | length == 1)
            and ([.to[] | select(
                .set_variable?.name == "finder_visual_mode"
                and .set_variable.value == 0
            )] | length == 1)
            and finder_visual_conditions
        )
    ] | length == 2),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Visual Mode: Mark selection for move with x and return to Normal Mode"
            and ([.to[] | select(
                .shell_command? == "$HOME/.local/libexec/finder-vim/finder_action_marked.sh cut-current >/dev/null 2>&1; exec /usr/bin/truncate -s 0 $HOME/.local/state/finder-vim/finder_visual_anchor.txt"
            )] | length == 1)
            and ([.to[] | select(
                .set_variable?.name == "finder_visual_mode"
                and .set_variable.value == 0
            )] | length == 1)
            and finder_visual_conditions
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
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Press Esc to clear discontiguous Finder marks"
            and .from == {"key_code":"escape"}
            and .to[0] == {"shell_command":$clear_selection_command}
            and .to[1] == {"set_variable":{"name":"finder_confirmed_marks_maybe_present","value":0}}
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
            and .to == [
                {"set_variable":{"name":"finder_confirmed_marks_maybe_present","value":1}},
                {"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_ax_step toggle-mark >/dev/null 2>&1"}
            ]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Copy confirmed marks, or selection when no marks exist, with y"
            and .from == {"key_code":"y"}
            and .to == [
                {"shell_command":$copy_marks_command},
                {"set_variable":{"name":"finder_cut_pending","value":0}},
                {"set_variable":{"name":"finder_copy_pending","value":1}}
            ]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Cut confirmed marks, or selection when no marks exist, with x"
            and .from == {"key_code":"x"}
            and .to == [
                {"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_action_marked.sh cut >/dev/null 2>&1"},
                {"set_variable":{"name":"finder_cut_pending","value":1}},
                {"set_variable":{"name":"finder_copy_pending","value":0}}
            ]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Trash confirmed marks, or selection when no marks exist, with d"
            and .from == {"key_code":"d"}
            and .to == [{"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_action_marked.sh delete >/dev/null 2>&1"}]
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Copy-paste Finer items with p after y"
            and .from == {"key_code":"p"}
            and .to == [{"shell_command":"exec $HOME/.local/libexec/finder-vim/finder_paste.sh copy >/dev/null 2>&1"}]
            and ([.conditions[] | select(
                .name == "finder_copy_pending"
                and .type == "variable_if"
                and .value == 1
            )] | length == 1)
            and ([.conditions[] | select(
                .name == "finder_cut_pending"
                and .type == "variable_unless"
                and .value == 1
            )] | length == 1)
            and finder_normal_conditions
        )
    ] | length == 1),
    ([
        .rules[]
        | select(.description == "Finer Navigation")
        | .manipulators[]
        | select(
            .description == "Normal Mode: Paste Finder items with p"
            and ([.conditions[] | select(
                .name == "finder_copy_pending"
                and .type == "variable_unless"
                and .value == 1
            )] | length == 1)
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
            or . == $visual_start_command
            or . == $copy_marks_command)
    ] | all)
] | all
' "$rule_file" >/dev/null

print -- "Generated rule regression tests passed."
