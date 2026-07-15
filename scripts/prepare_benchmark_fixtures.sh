#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
profile="${FINDER_VIM_FIXTURE_PROFILE:-empty-files}"
counts_string="${FINDER_VIM_BENCHMARK_COUNTS:-10 1000 10000}"
counts=("${(@s: :)counts_string}")

case "$profile" in
    empty-files)
        default_destination="$repo_root/.build/benchmark-fixtures/column-race"
        ;;
    realistic-mixed)
        default_destination="$repo_root/.build/benchmark-fixtures/realistic-mixed"
        ;;
    *)
        print -u2 -- "Unsupported fixture profile: $profile"
        exit 64
        ;;
esac

destination="${1:-$default_destination}"
for count in "${counts[@]}"; do
    if [[ "$count" != 10 && "$count" != 1000 && "$count" != 10000 ]]; then
        print -u2 -- "Unsupported item count: $count"
        exit 64
    fi
done

mkdir -p "$destination"
profile_marker="$destination/.content-profile"
if [[ -r "$profile_marker" ]]; then
    recorded_profile="$(<"$profile_marker")"
    if [[ "$recorded_profile" != "$profile" ]]; then
        print -u2 -- "Fixture profile mismatch in $destination: $recorded_profile"
        exit 1
    fi
else
    print -r -- "$profile" > "$profile_marker"
fi

fail_stale_item() {
    print -u2 -- "Unexpected existing fixture item: $1"
    print -u2 -- "Remove the generated fixture directory and retry."
    exit 1
}

copy_seed() {
    local seed="$1"
    local target="$2"
    local seed_size target_size

    seed_size="$(stat -f '%z' "$seed")"
    if [[ -e "$target" ]]; then
        [[ -f "$target" ]] || fail_stale_item "$target"
        target_size="$(stat -f '%z' "$target")"
        [[ "$target_size" == "$seed_size" ]] || fail_stale_item "$target"
        return
    fi
    cp "$seed" "$target"
}

prepare_realistic_seeds() {
    seed_dir="$destination/.fixture-seeds"
    mkdir -p "$seed_dir"
    text_seed="$seed_dir/text-4k.txt"
    large_text_seed="$seed_dir/text-16k.txt"
    binary_seed="$seed_dir/binary-64k.dat"
    image_seed="$seed_dir/image-128.ppm"
    png_seed="$seed_dir/image-1.png"
    json_seed="$seed_dir/metadata.json"
    rtf_seed="$seed_dir/document.rtf"

    if [[ ! -e "$text_seed" ]]; then
        {
            for ((line = 1; line <= 64; ++line)); do
                printf 'Finer benchmark text line %04d: deterministic local content.\n' "$line"
            done
        } > "$text_seed"
    fi
    [[ -f "$text_seed" && -s "$text_seed" ]] || fail_stale_item "$text_seed"

    if [[ ! -e "$large_text_seed" ]]; then
        {
            for ((line = 1; line <= 256; ++line)); do
                printf 'Finer benchmark log line %04d: deterministic local content for Finder metadata.\n' "$line"
            done
        } > "$large_text_seed"
    fi
    [[ -f "$large_text_seed" && -s "$large_text_seed" ]] || fail_stale_item "$large_text_seed"

    if [[ ! -e "$binary_seed" ]]; then
        dd if=/dev/zero of="$binary_seed" bs=65536 count=1 2>/dev/null
    fi
    [[ -f "$binary_seed" && "$(stat -f '%z' "$binary_seed")" == 65536 ]] \
        || fail_stale_item "$binary_seed"

    if [[ ! -e "$image_seed" ]]; then
        {
            print -r -- 'P6'
            print -r -- '128 128'
            print -r -- '255'
            dd if=/dev/zero bs=49152 count=1 2>/dev/null
        } > "$image_seed"
    fi
    [[ -f "$image_seed" && "$(stat -f '%z' "$image_seed")" == 49167 ]] \
        || fail_stale_item "$image_seed"

    if [[ ! -e "$png_seed" ]]; then
        print -rn -- 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=' \
            | base64 -D > "$png_seed"
    fi
    [[ -f "$png_seed" && -s "$png_seed" ]] || fail_stale_item "$png_seed"

    if [[ ! -e "$json_seed" ]]; then
        {
            print -r -- '{'
            print -r -- '  "profile": "realistic-mixed",'
            print -r -- '  "description": "Deterministic local Finder benchmark content",'
            print -r -- '  "items": ["text", "image", "binary", "folder"]'
            print -r -- '}'
        } > "$json_seed"
    fi
    [[ -f "$json_seed" && -s "$json_seed" ]] || fail_stale_item "$json_seed"

    if [[ ! -e "$rtf_seed" ]]; then
        print -r -- '{\rtf1\ansi\deff0 Finer deterministic Finder benchmark document.}' \
            > "$rtf_seed"
    fi
    [[ -f "$rtf_seed" && -s "$rtf_seed" ]] || fail_stale_item "$rtf_seed"
}

prepare_empty_files() {
    local child_dir="$1"
    local count="$2"
    local index filename
    typeset -a batch=()

    for ((index = 0; index < count; ++index)); do
        printf -v filename 'item-%05d.txt' "$index"
        batch+=("$child_dir/$filename")
        if (( ${#batch[@]} == 200 )); then
            touch "${batch[@]}"
            batch=()
        fi
    done
    if (( ${#batch[@]} > 0 )); then
        touch "${batch[@]}"
    fi
}

prepare_realistic_items() {
    local child_dir="$1"
    local count="$2"
    local index stem target nested

    for ((index = 0; index < count; ++index)); do
        printf -v stem 'item-%05d' "$index"
        if (( index < 4 )); then
            copy_seed "$text_seed" "$child_dir/$stem.txt"
            continue
        fi

        case $(( index % 10 )) in
            0) copy_seed "$text_seed" "$child_dir/$stem.txt" ;;
            1) copy_seed "$large_text_seed" "$child_dir/$stem notes.md" ;;
            2) copy_seed "$binary_seed" "$child_dir/$stem payload.dat" ;;
            3) copy_seed "$image_seed" "$child_dir/$stem image.ppm" ;;
            4) copy_seed "$png_seed" "$child_dir/$stem-画像.png" ;;
            5)
                target="$child_dir/$stem Folder"
                if [[ -e "$target" && ! -d "$target" ]]; then
                    fail_stale_item "$target"
                fi
                mkdir -p "$target"
                nested="$target/README.txt"
                copy_seed "$text_seed" "$nested"
                ;;
            6) copy_seed "$json_seed" "$child_dir/$stem metadata.json" ;;
            7) copy_seed "$rtf_seed" "$child_dir/$stem document.rtf" ;;
            8) copy_seed "$large_text_seed" "$child_dir/$stem activity.log" ;;
            9)
                target="$child_dir/$stem empty.txt"
                if [[ -e "$target" ]]; then
                    [[ -f "$target" && ! -s "$target" ]] || fail_stale_item "$target"
                else
                    : > "$target"
                fi
                ;;
        esac
    done
}

if [[ "$profile" == realistic-mixed ]]; then
    prepare_realistic_seeds
fi

typeset -a manifest_rows=()
for count in "${counts[@]}"; do
    case_dir="$destination/items-$count"
    child_dir="$case_dir/01-A"
    mkdir -p "$child_dir"
    touch "$case_dir/00-start.txt" "$case_dir/02-sibling.txt"

    case "$profile" in
        empty-files) prepare_empty_files "$child_dir" "$count" ;;
        realistic-mixed) prepare_realistic_items "$child_dir" "$count" ;;
    esac

    actual_count="$(find "$child_dir" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')"
    file_count="$(find "$child_dir" -mindepth 1 -maxdepth 1 -type f | wc -l | tr -d ' ')"
    directory_count="$(find "$child_dir" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')"
    logical_bytes="$(find "$child_dir" -type f -exec stat -f '%z' {} + | awk '{ total += $1 } END { print total + 0 }')"
    if [[ "$actual_count" != "$count" ]]; then
        print -u2 -- "Unexpected item count in $child_dir: $actual_count"
        print -u2 -- "Remove stale files from this generated fixture and retry."
        exit 1
    fi
    manifest_rows+=("$profile"$'\t'"$count"$'\t'"$actual_count"$'\t'"$file_count"$'\t'"$directory_count"$'\t'"$logical_bytes")
    print -- "Prepared $case_dir ($profile, $count items, $logical_bytes logical bytes)"
done

{
    print -r -- $'profile\tcount\tvisible_items\tfiles\tdirectories\tlogical_bytes'
    for row in "${manifest_rows[@]}"; do
        print -r -- "$row"
    done
} > "$destination/manifest.tsv"
