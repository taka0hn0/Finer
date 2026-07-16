#!/bin/zsh
set -euo pipefail

helper="${1:-}"
if [[ -z "$helper" || ! -x "$helper" ]]; then
    print -u2 -- "Missing executable benchmark helper: $helper"
    exit 1
fi

binary_strings="$(/usr/bin/strings "$helper")"
if [[ "$binary_strings" != *FINDER_VIM_METRICS_FILE* ]]; then
    print -u2 -- "Installed helper does not expose benchmark metrics: $helper"
    print -u2 -- "Use a metrics-capable benchmark build. Finder was not opened."
    exit 1
fi
