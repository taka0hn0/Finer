#!/bin/zsh
set -euo pipefail
zmodload zsh/datetime

if (( $# != 8 )); then
    print -u2 -- \
        "Usage: $0 HELPER STATE_ROOT INTERVAL_MS SCENARIO ITERATION DIRECTIONS KEYS SAMPLES_FILE"
    exit 64
fi

helper="$1"
state_root="$2"
interval_ms="$3"
scenario="$4"
iteration="$5"
directions_string="$6"
keys_string="$7"
samples_file="$8"

if [[ ! -x "$helper" ]]; then
    print -u2 -- "Missing executable helper: $helper"
    exit 1
fi
if [[ ! "$interval_ms" =~ '^[1-9][0-9]*$'
    || "$interval_ms" -lt 20 || "$interval_ms" -gt 1000 ]]; then
    print -u2 -- "Tap interval must be between 20 and 1000ms: $interval_ms"
    exit 64
fi
if [[ ! "$scenario" =~ '^[A-Za-z0-9][A-Za-z0-9._-]*$' ]]; then
    print -u2 -- "Invalid scenario name: $scenario"
    exit 64
fi
if [[ ! "$iteration" =~ '^[1-9][0-9]*$' ]]; then
    print -u2 -- "Iteration must be a positive integer: $iteration"
    exit 64
fi
if [[ ! -f "$samples_file" || ! -w "$samples_file" ]]; then
    print -u2 -- "Samples file must already exist and be writable: $samples_file"
    exit 1
fi

typeset -a directions=("${(s: :)directions_string}")
typeset -a keys=("${(s: :)keys_string}")
if (( ${#directions[@]} == 0 || ${#directions[@]} != ${#keys[@]} )); then
    print -u2 -- "Direction/key sequence length mismatch: $scenario"
    exit 64
fi

for direction in "${directions[@]}"; do
    case "$direction" in
        down|up|left|right) ;;
        *)
            print -u2 -- "Unsupported direction in $scenario: $direction"
            exit 64
            ;;
    esac
    token_file="$state_root/finder_${direction}_hold.txt"
    if [[ ! -f "$token_file" ]]; then
        print -u2 -- "Missing hold token: $token_file"
        exit 1
    fi
done
for key in "${keys[@]}"; do
    case "$key" in
        h|j|k|l) ;;
        *)
            print -u2 -- "Unsupported key in $scenario: $key"
            exit 64
            ;;
    esac
done

typeset -F 9 interval_seconds=$(( interval_ms / 1000.0 ))
typeset -F 9 burst_started target_time now remaining
typeset -F 9 submitted_time finished_time
burst_started=$EPOCHREALTIME
command_failures=0

for ((step = 1; step <= ${#directions[@]}; ++step)); do
    direction="${directions[$step]}"
    key="${keys[$step]}"
    target_time=$(( burst_started + (step - 1) * interval_seconds ))
    now=$EPOCHREALTIME
    remaining=$(( target_time - now ))
    if (( remaining > 0 )); then
        sleep "$remaining"
    fi

    submitted_time=$EPOCHREALTIME
    client_status=0
    "$helper" hold-start "$direction" >/dev/null 2>&1 \
        || client_status=$?
    finished_time=$EPOCHREALTIME
    /usr/bin/truncate -s 0 "$state_root/finder_${direction}_hold.txt"
    if (( client_status != 0 )); then
        (( ++command_failures ))
    fi
    print -r -- "$scenario"$'\t'"$iteration"$'\t'"$step"$'\t'"$key"$'\t'"$direction"$'\t'"$target_time"$'\t'"$submitted_time"$'\t'"$finished_time"$'\t'"$client_status" \
        >> "$samples_file"
done

(( command_failures == 0 ))
