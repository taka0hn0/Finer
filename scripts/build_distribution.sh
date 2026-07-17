#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
output_root="${FINER_DIST_DIR:-$repo_root/.build/dist}"
source_ref="${FINER_DIST_REF:-HEAD}"
version="${1:-}"

fail() {
    print -u2 -- "finer dist: $1"
    exit 1
}

if [[ "$source_ref" == -* || "$source_ref" == *[[:space:]]* ]]; then
    fail "invalid source ref: $source_ref"
fi

if [[ -z "$version" ]]; then
    short_commit="$(git -C "$repo_root" rev-parse --verify --short=12 "$source_ref^{commit}")" \
        || fail "cannot resolve source ref: $source_ref"
    version="dev-$short_commit"
fi
if [[ ! "$version" =~ '^[0-9A-Za-z][0-9A-Za-z._-]*$' ]]; then
    fail "invalid version: $version"
fi

commit="$(git -C "$repo_root" rev-parse --verify "$source_ref^{commit}")" \
    || fail "cannot resolve source ref: $source_ref"
if [[ "${FINER_DIST_ALLOW_DIRTY:-0}" != 1 ]] \
    && [[ -n "$(git -C "$repo_root" status --porcelain --untracked-files=no)" ]]; then
    fail "tracked files are modified; commit them or set FINER_DIST_ALLOW_DIRTY=1 to archive the unchanged ref"
fi
if [[ -L "$output_root" ]]; then
    fail "refusing symlinked output directory: $output_root"
fi

archive_root="Finer-$version"
archive_name="$archive_root.tar.gz"
archive_path="$output_root/$archive_name"
checksum_path="$archive_path.sha256"
mkdir -p "$output_root"

for output_path in "$archive_path" "$checksum_path"; do
    if [[ -L "$output_path" ]]; then
        fail "refusing symlinked output file: $output_path"
    fi
done

temp_tar="$output_root/.$archive_name.$$.tar"
temp_archive="$output_root/.$archive_name.$$.tmp"
temp_checksum="$output_root/.$archive_name.$$.sha256.tmp"
cleanup() {
    rm -f "$temp_tar" "$temp_archive" "$temp_checksum"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

umask 022
COPYFILE_DISABLE=1 git -C "$repo_root" archive \
    --format=tar \
    --prefix="$archive_root/" \
    --output="$temp_tar" \
    "$commit"
gzip -n -9 -c "$temp_tar" > "$temp_archive"
archive_hash="$(shasum -a 256 "$temp_archive" | awk '{ print $1 }')"
print -r -- "$archive_hash  $archive_name" > "$temp_checksum"

/bin/mv -f "$temp_archive" "$archive_path"
/bin/mv -f "$temp_checksum" "$checksum_path"

print -- "Archive: $archive_path"
print -- "Checksum: $checksum_path"
print -- "Commit: $commit"
print -- "SHA-256: $archive_hash"
