#!/usr/bin/env bash
# Build all client mod DLLs (Rust) and devmenu_imgui.dll (C++ ImGui overlay)
# inside the Humble-ROS2 Distrobox, which has the mingw64 cross-compiler.
#
# Run this from the HOST (it wraps distrobox-run automatically):
#
#   bash modding-toolkit/local/build_client_mods.sh
#
# To build only one component:
#   bash build_client_mods.sh rust    # Rust DLLs only
#   bash build_client_mods.sh imgui   # devmenu_imgui.dll only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMGUI_DIR="${SCRIPT_DIR}/mods/client/devmenu_imgui"

DISTROBOX="Humble-ROS2"

# Cargo binary lives in ~/.cargo/bin; ~/cargo-bin is a symlink there so
# the Distrobox environment (which may have a different PATH) can find it.
CARGO_BIN="/home/doobs/cargo-bin/cargo"

COMPONENT="${1:-all}"

build_rust() {
    echo "==> Building Rust client mod DLLs in ${DISTROBOX}…"
    distrobox-run --name "${DISTROBOX}" -- bash -lc "
        set -euo pipefail
        cd '${SCRIPT_DIR}'
        '${CARGO_BIN}' build --release 2>&1
    "
    echo "==> Rust build done.  DLLs in: ${SCRIPT_DIR}/target/x86_64-pc-windows-gnu/release/"
}

build_imgui() {
    echo "==> Building devmenu_imgui.dll in ${DISTROBOX}…"
    distrobox-run --name "${DISTROBOX}" -- bash -lc "
        set -euo pipefail
        cd '${IMGUI_DIR}'
        bash build_mingw.sh 2>&1
    "
    echo "==> ImGui overlay build done.  DLL in: ${IMGUI_DIR}/build/devmenu_imgui.dll"
}

deploy() {
    local MODS_DIR="/home/doobs/.local/share/Steam/steamapps/common/Spellbreak/Mods/dlls"
    mkdir -p "${MODS_DIR}"

    echo "==> Deploying to ${MODS_DIR}…"

    # Rust DLLs — copy every cdylib from the release output
    local RELEASE_DIR="${SCRIPT_DIR}/target/x86_64-pc-windows-gnu/release"
    if [[ -d "${RELEASE_DIR}" ]]; then
        for dll in "${RELEASE_DIR}"/*.dll; do
            [[ -e "${dll}" ]] || continue
            echo "    $(basename "${dll}")"
            cp "${dll}" "${MODS_DIR}/"
        done
    fi

    # ImGui overlay DLL
    local IMGUI_DLL="${IMGUI_DIR}/build/devmenu_imgui.dll"
    if [[ -f "${IMGUI_DLL}" ]]; then
        echo "    devmenu_imgui.dll"
        cp "${IMGUI_DLL}" "${MODS_DIR}/"
    fi

    echo "==> Deploy complete."
}

case "${COMPONENT}" in
    rust)  build_rust  ;;
    imgui) build_imgui ;;
    all)
        build_rust
        build_imgui
        deploy
        ;;
    *)
        echo "Unknown component '${COMPONENT}'.  Use: rust | imgui | all"
        exit 1
        ;;
esac
