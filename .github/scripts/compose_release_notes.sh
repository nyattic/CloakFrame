#!/usr/bin/env bash
# Combines the per-language release notes for one version into a single document.
#
# The Korean notes come first so the GitHub release page opens with them, and the other
# languages are folded into <details> blocks. Each section is introduced by an HTML comment
# marker, which GitHub does not render but the in-app updater uses to show a user only the
# language their interface is in. Keep the markers in sync with
# `kReleaseNotesLanguageMarker` in include/cloakframe/ReleaseNotes.hpp.
#
# Usage: compose_release_notes.sh <version> <output-file>
set -euo pipefail

version="${1:?usage: compose_release_notes.sh <version> <output-file>}"
output="${2:?usage: compose_release_notes.sh <version> <output-file>}"

for language in ko en ja zh; do
    notes="release-notes/${version}.${language}.md"
    if [[ ! -s "$notes" ]]; then
        echo "Missing release notes: $notes" >&2
        exit 1
    fi
done

{
    printf '<!-- notes:ko -->\n\n'
    cat "release-notes/${version}.ko.md"
    printf '\n<details><summary>English</summary>\n\n<!-- notes:en -->\n\n'
    cat "release-notes/${version}.en.md"
    printf '\n</details>\n\n<details><summary>日本語</summary>\n\n<!-- notes:ja -->\n\n'
    cat "release-notes/${version}.ja.md"
    printf '\n</details>\n\n<details><summary>简体中文</summary>\n\n<!-- notes:zh -->\n\n'
    cat "release-notes/${version}.zh.md"
    printf '\n</details>\n'
} > "$output"
