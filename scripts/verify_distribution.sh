#!/bin/zsh
set -euo pipefail

archive="${1:-}"
if [[ -z "$archive" ]]; then
    print -u2 -- "Usage: $0 ARCHIVE.tar.gz"
    exit 64
fi
archive="${archive:A}"
checksum="$archive.sha256"

fail() {
    print -u2 -- "finer dist verify: $1"
    exit 1
}

if [[ ! -f "$archive" || ! -r "$archive" || -L "$archive" ]]; then
    fail "archive must be a readable regular file: $archive"
fi
if [[ ! -f "$checksum" || ! -r "$checksum" || -L "$checksum" ]]; then
    fail "checksum must be a readable regular file: $checksum"
fi

expected_hash="$(awk 'NR == 1 { print $1 }' "$checksum")"
expected_name="$(awk 'NR == 1 { print $2 }' "$checksum")"
if [[ ! "$expected_hash" =~ '^[0-9a-f]{64}$' ]]; then
    fail "invalid SHA-256 file: $checksum"
fi
if [[ "$expected_name" != "${archive:t}" ]]; then
    fail "checksum names $expected_name instead of ${archive:t}"
fi
actual_hash="$(shasum -a 256 "$archive" | awk '{ print $1 }')"
if [[ "$actual_hash" != "$expected_hash" ]]; then
    fail "checksum mismatch: expected=$expected_hash actual=$actual_hash"
fi

typeset -a entries=("${(@f)$(tar -tzf "$archive")}")
if (( ${#entries[@]} == 0 )); then
    fail "archive is empty"
fi
archive_root="${entries[1]%/}"
if [[ -z "$archive_root" || "$archive_root" == */* || "$archive_root" == .* ]]; then
    fail "invalid archive root: ${entries[1]}"
fi
for entry in "${entries[@]}"; do
    if [[ "$entry" == /* || "$entry" == *'/../'* || "$entry" == *'/..' \
        || "$entry" == '../'* \
        || ( "$entry" != "$archive_root" && "$entry" != "$archive_root/"* ) ]]; then
        fail "unsafe archive entry: $entry"
    fi
done
if tar -tvzf "$archive" | awk '
    substr($1, 1, 1) != "-" && substr($1, 1, 1) != "d" { found = 1 }
    END { exit !found }
'; then
    fail "archive contains a symlink or special file"
fi

temp_root="$(mktemp -d "${TMPDIR:-/tmp}/finer-dist-verify.XXXXXX")"
cleanup() {
    rm -rf "$temp_root"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

COPYFILE_DISABLE=1 tar -xzf "$archive" -C "$temp_root"
source_root="$temp_root/$archive_root"
for required in Makefile README.md LICENSE scripts/install.sh scripts/uninstall.sh \
    scripts/build_distribution.sh scripts/verify_distribution.sh \
    scripts/test_distribution.sh rules/generated/finder-vim.json \
    src/finder_ax_step.c src/finder_ax_move.swift; do
    if [[ ! -f "$source_root/$required" ]]; then
        fail "missing required source file: $required"
    fi
done
if [[ -e "$source_root/.git" ]]; then
    fail "archive unexpectedly contains .git"
fi

make -C "$source_root" check
make -C "$source_root" test-install
print -- "Distribution verified: ${archive:t} ($actual_hash)"
