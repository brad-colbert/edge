# scripts/lib/atari_emu.sh — shared Altirra/wine helpers for the run_* scripts.
#
# Source this (do not execute it):  source "$REPO/scripts/lib/atari_emu.sh"
# It expects $REPO to be set by the caller (the repo root).
#
# Locating Altirra is deliberately layout-agnostic so a fresh clone works
# without editing anything: set ALTIRRA_DIR to point at the install, otherwise
# a list of common locations is searched. The original author's Dropbox path is
# kept last for back-compat.

# Echo the Altirra install directory (the dir containing Altirra64.exe), or
# return non-zero with a message on stderr if none is found.
edge_find_altirra() {
    # 1. Explicit override always wins.
    if [ -n "${ALTIRRA_DIR:-}" ]; then
        if [ -d "$ALTIRRA_DIR" ]; then
            printf '%s' "${ALTIRRA_DIR%/}"
            return 0
        fi
        echo "ALTIRRA_DIR=$ALTIRRA_DIR is not a directory" >&2
        return 2
    fi

    # 2. Search common locations (newest versioned dir wins within each glob).
    local pat dir
    for pat in \
        "$REPO/third_party"/Altirra*/ \
        "$HOME"/Altirra*/ \
        /opt/Altirra*/ \
        "$HOME"/Dropbox/Projects/Atari/Altirra/Altirra-*/
    do
        dir="$(ls -d $pat 2>/dev/null | sort -V | tail -n 1)"
        if [ -n "$dir" ] && [ -d "$dir" ]; then
            printf '%s' "${dir%/}"
            return 0
        fi
    done

    # 3. Altirra64.exe on PATH (e.g. a wrapper dir).
    local onpath
    onpath="$(command -v Altirra64.exe 2>/dev/null || true)"
    if [ -n "$onpath" ]; then
        printf '%s' "$(dirname "$onpath")"
        return 0
    fi

    echo "Altirra not found; set ALTIRRA_DIR=/path/to/Altirra-install" >&2
    return 2
}

# Convert a host path to the wine Z: drive form Altirra expects.
edge_to_wine() {
    printf 'Z:%s' "$(printf '%s' "$1" | tr '/' '\\')"
}
