#!/usr/bin/env bash
# Build all client mod DLLs (Rust) and devmenu_qt.dll (C++ Qt) inside the
# Humble-ROS2 Distrobox, which has the mingw64 cross-compiler.
#
# Run this from the HOST (it wraps distrobox-run automatically):
#
#   bash modding-toolkit/local/build_client_mods.sh
#
# To build only one component:
#   bash build_client_mods.sh rust      # Rust DLLs only
#   bash build_client_mods.sh qt        # devmenu_qt.dll only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QT_UI_DIR="${SCRIPT_DIR}/mods/client/qt_devmenu/qt_ui"

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

build_qt() {
    echo "==> Building devmenu_qt.dll in ${DISTROBOX}…"
    distrobox-run --name "${DISTROBOX}" -- bash -lc "
        set -euo pipefail
        cd '${QT_UI_DIR}'
        bash build_mingw.sh 2>&1
    "
    echo "==> Qt build done.  DLL in: ${QT_UI_DIR}/build/devmenu_qt.dll"
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

    # Qt DLL
    local QT_DLL="${QT_UI_DIR}/build/devmenu_qt.dll"
    if [[ -f "${QT_DLL}" ]]; then
        echo "    devmenu_qt.dll"
        cp "${QT_DLL}" "${MODS_DIR}/"
    fi

    echo "==> Deploy complete."
}

case "${COMPONENT}" in
    rust) build_rust ;;
    qt)   build_qt   ;;
    all)
        build_rust
        build_qt
        deploy
        ;;
    *)
        echo "Unknown component '${COMPONENT}'.  Use: rust | qt | all"
        exit 1
        ;;
esac
