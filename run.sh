#!/usr/bin/env bash
# Linux entry point: builds the port when needed and runs run.py, which prepares the game image
# and starts out/gravityrush-runtime (same environment variables: GOW3_GAME_DIR, GOW3_DATA_DIR,
# GOW3_PATCHES, GOW3_MODS_*, GOW3_USER_DIR, ...). `bash run.sh --software` renders with lavapipe.
# GOW3_PREBUILT=1 (packaged builds, the AppImage): the runtime is installed in bin/; nothing is
# built and no nix-shell is needed.
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ -z ${GOW3_PREBUILT:-} && -z ${GOW3_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env GOW3_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
if [[ -z ${PYTHON:-} ]]; then
    PYTHON=$(command -v python3 || true)
    if [[ -z $PYTHON ]]; then
        for candidate in /nix/store/*-python3-*/bin/python3; do
            if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
        done
    fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
if [[ -z ${GOW3_PREBUILT:-} && -z ${GOW3_PROBE:-} ]]; then
    bash build.sh
fi
exec "$PYTHON" run.py "$@"
