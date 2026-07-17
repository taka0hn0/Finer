#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
source_ref="${FINER_DIST_REF:-HEAD}"
short_commit="$(git -C "$repo_root" rev-parse --short=12 "$source_ref^{commit}")"
version="test-$short_commit"
temp_root="$(mktemp -d "${TMPDIR:-/tmp}/finer-dist-test.XXXXXX")"
cleanup() {
    rm -rf "$temp_root"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for label in first second; do
    FINER_DIST_DIR="$temp_root/$label" \
    FINER_DIST_REF="$source_ref" \
    FINER_DIST_ALLOW_DIRTY="${FINER_DIST_ALLOW_DIRTY:-0}" \
        "$repo_root/scripts/build_distribution.sh" "$version" >/dev/null
done

first_archive="$temp_root/first/Finer-$version.tar.gz"
second_archive="$temp_root/second/Finer-$version.tar.gz"
if ! cmp -s "$first_archive" "$second_archive"; then
    print -u2 -- "Distribution archives are not reproducible"
    exit 1
fi
if ! cmp -s "$first_archive.sha256" "$second_archive.sha256"; then
    print -u2 -- "Distribution checksum files are not reproducible"
    exit 1
fi

"$repo_root/scripts/verify_distribution.sh" "$first_archive"

tampered_root="$temp_root/tampered"
mkdir -p "$tampered_root"
/bin/cp "$first_archive" "$tampered_root/${first_archive:t}"
/bin/cp "$first_archive.sha256" "$tampered_root/${first_archive:t}.sha256"
print -n -- x >> "$tampered_root/${first_archive:t}"
if "$repo_root/scripts/verify_distribution.sh" \
    "$tampered_root/${first_archive:t}" >/dev/null 2>&1; then
    print -u2 -- "Tampered distribution unexpectedly passed verification"
    exit 1
fi

print -- "Distribution reproducibility and tamper tests passed."
