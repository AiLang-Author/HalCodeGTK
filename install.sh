#!/usr/bin/env bash
# HalCodeGTK one-shot install: build the GTK desk, menu entry, keys.
# Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
#
# Usage:
#   ./install.sh
#   ./install.sh --skip-keys --skip-postgres --skip-olympus
#   ./install.sh --no-build          # desktop + keys only
#   ./install.sh --no-desktop
set -euo pipefail

SKIP_KEYS=0
SKIP_POSTGRES=0
SKIP_OLYMPUS=0
NO_BUILD=0
NO_DESKTOP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-keys) SKIP_KEYS=1; shift ;;
        --skip-postgres) SKIP_POSTGRES=1; shift ;;
        --skip-olympus) SKIP_OLYMPUS=1; shift ;;
        --no-build) NO_BUILD=1; shift ;;
        --no-desktop) NO_DESKTOP=1; shift ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
export HALCODE_ROOT="$ROOT"

log()  { printf '==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

if [[ -z "${AILANG_ROOT:-}" ]]; then
    if [[ -d "$HOME/Ailang-Self-Hosting-" ]]; then
        AILANG_ROOT="$HOME/Ailang-Self-Hosting-"
    elif command -v ailang.x >/dev/null 2>&1; then
        AILANG_ROOT="$(cd "$(dirname "$(readlink -f "$(command -v ailang.x)")")" && pwd)"
    else
        AILANG_ROOT=""
    fi
fi
export AILANG_ROOT

if [[ "$NO_BUILD" -eq 0 ]]; then
    command -v cc >/dev/null 2>&1 || die "cc not found (install gcc or clang)"
    command -v pkg-config >/dev/null 2>&1 || die "pkg-config not found"
    pkg-config --exists gtk+-3.0 cairo || die "GTK 3 + cairo not found (Debian: sudo apt install libgtk-3-dev libcairo2-dev pkg-config)"
    command -v ailang.x >/dev/null 2>&1 || die "ailang.x not on PATH. Clone https://github.com/AiLang-Author/Ailang-Self-Hosting- and run its install_compiler.sh"

    log "Building agent + tools"
    ./build.sh

    log "Building GTK host + desk (AILANG_ROOT=$AILANG_ROOT)"
    make -C shell AILANG_ROOT="$AILANG_ROOT"
    [[ -x "$ROOT/shell/halcode_desk.x" ]] || die "desk build failed"
    [[ -x "$ROOT/shell/halcode_shell_gtk" ]] || die "GTK host build failed"
    [[ -x "$ROOT/HalCode9000.x" ]] || die "agent binary missing"
fi

if [[ "$NO_DESKTOP" -eq 0 ]]; then
    log "Applications menu + ~/.local/bin/halcode"
    chmod +x "$ROOT/scripts/launch_halcode.sh" "$ROOT/scripts/install_desktop.sh"
    "$ROOT/scripts/install_desktop.sh"
fi

log "Keys / pgmem / Olympus (same as TUI)"
SETUP_ARGS=()
[[ "$SKIP_KEYS" -eq 1 ]] && SETUP_ARGS+=(--skip-keys)
[[ "$SKIP_POSTGRES" -eq 1 ]] && SETUP_ARGS+=(--skip-postgres)
[[ "$SKIP_OLYMPUS" -eq 1 ]] && SETUP_ARGS+=(--skip-olympus)
"$ROOT/setup.sh" "${SETUP_ARGS[@]}"

if [[ ! -f "$HOME/.halcode/system_prompt.txt" && -f "$ROOT/config/system_prompt.txt" ]]; then
    mkdir -p "$HOME/.halcode"
    chmod 700 "$HOME/.halcode"
    cp "$ROOT/config/system_prompt.txt" "$HOME/.halcode/system_prompt.txt"
    if [[ ! -f "$HOME/.halcode/halcode.json" ]]; then
        printf '%s\n' '{ "max_tokens": 16384, "system_prompt_file": "system_prompt.txt" }' \
            > "$HOME/.halcode/halcode.json"
    fi
    info "wrote ~/.halcode/system_prompt.txt"
fi

log "Done"
info "Launch:  halcode"
info "Or:      $ROOT/scripts/launch_halcode.sh"
info "Prompt:  ~/.halcode/system_prompt.txt"
info "Rebuild: make -C $ROOT && halcode"
