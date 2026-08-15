#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
helper="$repo_root/.build/finer_open_panel"
rule="$repo_root/rules/generated/finder-vim.json"

"$helper" self-test >/dev/null

jq -e '
    def vscode_only:
        ([.conditions[] | select(
            .type == "frontmost_application_if"
            and .bundle_identifiers == ["^com\\.microsoft\\.VSCode$"]
        )] | length == 1);
    def non_text:
        ([.conditions[] | select(
            .type == "expression_unless"
            and .expression == "accessibility.focused_ui_element.role_string like '\''AXText*'\''"
        )] | length == 1);
    def jump_guard:
        ([.conditions[] | select(
            .type == "variable_unless"
            and .name == "finer_jump_active"
            and .value == 1
        )] | length == 1);
    def active($value):
        ([.conditions[] | select(
            .type == (if $value == 1 then "variable_if" else "variable_unless" end)
            and .name == "finer_open_panel_active"
            and .value == 1
        )] | length == 1);
    def helper($command):
        .to == [{
            "shell_command": ("exec $HOME/.local/libexec/finder-vim/finer_open_panel handle " + $command + " >/dev/null 2>&1")
        }];
    def direct($from; $to):
        .from == $from
        and .to == [
            {"set_variable":{"name":"finer_open_panel_g_prefix","value":0}},
            $to
        ];

    [.rules[] | select(.description == "Finer Open Panel Navigation")]
    | length == 1
    and (.[0].manipulators | length == 30)
    and ([.[0].manipulators[] | vscode_only and non_text and jump_guard] | all)
    and ([.[0].manipulators[] | select(
        .description | contains("Validate then handle")
    ) | select(active(0))] | length == 14)
    and ([.[0].manipulators[] | select(
        .description | contains("uses native navigation")
    )] | length == 12)
    and ([.[0].manipulators[] | select(
        .description | contains("Complete gg")
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        .description | contains("Start gg")
    )] | length == 2)
    and ([.[0].manipulators[] | .from.key_code]
        | index("return_or_enter") == null and index("escape") == null)
    and ([.[0].manipulators[] | select(
        (.description | contains("Validate then handle h")) and helper("h")
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("Validate then handle G"))
        and .from.modifiers.mandatory == ["shift"] and helper("G")
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("h uses native navigation"))
        and active(1)
        and direct({"key_code":"h"}; {"key_code":"left_arrow","repeat":true})
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("j uses native navigation"))
        and direct({"key_code":"j"}; {"key_code":"down_arrow","repeat":true})
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("k uses native navigation"))
        and direct({"key_code":"k"}; {"key_code":"up_arrow","repeat":true})
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("l uses native navigation"))
        and direct({"key_code":"l"}; {"key_code":"right_arrow","repeat":true})
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("G uses native navigation"))
        and direct(
            {"key_code":"g","modifiers":{"mandatory":["shift"],"optional":["caps_lock"]}};
            {"key_code":"down_arrow","repeat":false,"modifiers":["option"]}
        )
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("o uses native navigation"))
        and direct({"key_code":"o"}; {"key_code":"return_or_enter","repeat":false})
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("Complete gg"))
        and .to[0] == {"key_code":"up_arrow","modifiers":["option"],"repeat":false}
        and ([.conditions[] | select(
            .type == "variable_if" and .name == "finer_open_panel_g_prefix" and .value == 1
        )] | length == 1)
    )] | length == 2)
    and ([.[0].manipulators[] | select(
        (.description | contains("Start gg"))
        and .parameters["basic.to_delayed_action_delay_milliseconds"] == 1000
        and .to_delayed_action.to_if_invoked == [{"set_variable":{"name":"finer_open_panel_g_prefix","value":0}}]
    )] | length == 2)
' "$rule" >/dev/null

if "$helper" handle x >/dev/null 2>&1; then
    print -u2 -- "Open panel helper accepted an invalid command"
    exit 1
fi

print -- "Open panel helper and rule tests passed."
