#!/usr/bin/env bash
# scripts/setup.sh — verify the EDGE build environment ("doctor").
#
# Read-only: this checks for the required toolchain and reports the exact remedy
# for anything missing. It does NOT install anything — see the install link
# below and the Requirements section of README.md.
#
# Exit status: 0 if every CORE tool is present, non-zero otherwise. Optional
# tools (Altirra/Wine for hardware validation) only warn.

set -uo pipefail

SDK_URL="https://github.com/llvm-mos/llvm-mos-sdk"
fail=0

say()  { printf '%s\n' "$*"; }
ok()   { printf '  \033[32mok\033[0m   %s\n' "$*"; }
bad()  { printf '  \033[31mMISS\033[0m %s\n' "$*"; fail=1; }
warn() { printf '  \033[33mwarn\033[0m %s\n' "$*"; }

# Resolve a compiler either on PATH or in the conventional /usr/local/bin.
have_tool() { command -v "$1" >/dev/null 2>&1 || [ -x "/usr/local/bin/$1" ]; }

say "== EDGE environment check =="
say ""
say "Core (required to build or test):"

# CMake >= 3.20
if command -v cmake >/dev/null 2>&1; then
    ver="$(cmake --version | head -n1 | awk '{print $3}')"
    if [ "$(printf '%s\n3.20\n' "$ver" | sort -V | head -n1)" = "3.20" ]; then
        ok "cmake $ver"
    else
        bad "cmake $ver is older than 3.20 — upgrade CMake (https://cmake.org/)"
    fi
else
    bad "cmake not found — install CMake >= 3.20 (https://cmake.org/)"
fi

# Python 3 (host-side tooling)
if command -v python3 >/dev/null 2>&1; then
    ok "python3 $(python3 -c 'import platform; print(platform.python_version())')"
else
    bad "python3 not found — install Python 3 (https://www.python.org/)"
fi

# llvm-mos toolchains
if have_tool mos-sim-clang++; then
    ok "mos-sim-clang++ (unit-test simulator toolchain)"
else
    bad "mos-sim-clang++ not found — install the llvm-mos-sdk into /usr/local: $SDK_URL"
fi

if have_tool mos-atari8-dos-clang++; then
    ok "mos-atari8-dos-clang++ (Atari .xex toolchain)"
else
    bad "mos-atari8-dos-clang++ not found — install the llvm-mos-sdk into /usr/local: $SDK_URL"
fi

say ""
say "Optional (Atari hardware/VBXE validation in an emulator):"
if command -v wine >/dev/null 2>&1; then ok "wine"; else warn "wine not found — needed to run Altirra on Linux"; fi
if ALTIRRA_DIR="${ALTIRRA_DIR:-}" REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" \
   bash -c 'source "$REPO/scripts/lib/atari_emu.sh"; edge_find_altirra >/dev/null 2>&1'; then
    ok "Altirra located"
else
    warn "Altirra not found — set ALTIRRA_DIR or see README Requirements"
fi

say ""
if [ "$fail" -eq 0 ]; then
    say "All core tools present. Next:  scripts/build_demos.sh"
else
    say "Missing core tools above. After installing the llvm-mos-sdk into /usr/local, re-run this script."
fi
exit "$fail"
