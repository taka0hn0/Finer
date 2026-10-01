#!/bin/zsh
set -euo pipefail
repo_root="${0:A:h:h}"
test_binary="$(mktemp "$repo_root/.build/navigation-selection-test.XXXXXX")"
trap 'rm -f "$test_binary"' EXIT
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror \
    -framework ApplicationServices -framework Carbon \
    "$repo_root/tests/navigation_selection.c" -o "$test_binary"
"$test_binary"
