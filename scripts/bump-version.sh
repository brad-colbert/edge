#!/usr/bin/env bash
#
# bump-version.sh — set the EDGE version everywhere from one command.
#
# Usage: scripts/bump-version.sh X.Y.Z
#
# Updates the canonical macros in engine/version.h and rewrites the
# "Applies to EDGE vX.Y.Z" stamp in every tracked document. It does NOT edit
# CHANGELOG.md or create a git tag — it prints a reminder to do both, since
# those steps need human-written release notes.
#
# CMakeLists.txt parses engine/version.h directly, so PROJECT_VERSION follows
# automatically with no edit here.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 X.Y.Z" >&2
    exit 2
fi

NEW="$1"
if [[ ! "$NEW" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "error: version must be MAJOR.MINOR.PATCH (e.g. 0.1.1), got '$NEW'" >&2
    exit 2
fi
MAJOR="${BASH_REMATCH[1]}"
MINOR="${BASH_REMATCH[2]}"
PATCH="${BASH_REMATCH[3]}"

# Resolve repo root from this script's location so it works from any cwd.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION_H="engine/version.h"

# 1. Canonical macros.
sed -i -E \
    -e "s/(#define EDGE_VERSION_MAJOR )[0-9]+/\1${MAJOR}/" \
    -e "s/(#define EDGE_VERSION_MINOR )[0-9]+/\1${MINOR}/" \
    -e "s/(#define EDGE_VERSION_PATCH )[0-9]+/\1${PATCH}/" \
    -e "s/(#define EDGE_VERSION_STRING ).*/\1\"${NEW}\"/" \
    "$VERSION_H"
echo "updated $VERSION_H -> $NEW"

# 2. Document stamps. Touch only the version token in the "Applies to" line so
#    the surrounding markdown (including the em-dash) is left untouched.
DOCS=(
    README.md
    documents/README.md
    documents/QUICK_START.md
    documents/API_REFERENCE.md
    documents/PLATFORM_ATARI.md
    docs/ARCHITECTURE.md
    docs/API_DESIGN.md
    docs/CONSTRAINTS.md
    docs/DECISIONS.md
    demo/README.md
)
for doc in "${DOCS[@]}"; do
    if [[ -f "$doc" ]] && grep -q "Applies to EDGE v" "$doc"; then
        sed -i -E "s/Applies to EDGE v[0-9]+\.[0-9]+\.[0-9]+/Applies to EDGE v${NEW}/" "$doc"
        echo "stamped $doc"
    else
        echo "warning: no stamp found in $doc (skipped)" >&2
    fi
done

cat <<EOF

Version bumped to ${NEW}. Remaining manual steps:
  1. Edit CHANGELOG.md: move the [Unreleased] notes into a new
     '## [${NEW}] - $(date +%Y-%m-%d)' section.
  2. Commit, then tag the release:
       git commit -am "Release ${NEW}"
       git tag v${NEW}
EOF
