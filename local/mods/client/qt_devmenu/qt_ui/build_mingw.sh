#!/usr/bin/env bash
# Build devmenu_qt.dll using the x86_64-w64-mingw32 cross-compiler.
#
# Usage (from this directory):
#   bash build_mingw.sh
#
# The resulting DLL will be at ./build/devmenu_qt.dll.
# Copy it to Mods/dlls/ in your Spellbreak install.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Toolchain file for x86_64-w64-mingw32 — adjust paths if your distro
# installs things differently.
TOOLCHAIN_FILE="${SCRIPT_DIR}/toolchain-mingw64.cmake"

if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
    cat > "${TOOLCHAIN_FILE}" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TRIPLE x86_64-w64-mingw32)

find_program(CMAKE_C_COMPILER   NAMES ${TRIPLE}-gcc   REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${TRIPLE}-g++   REQUIRED)
find_program(CMAKE_RC_COMPILER  NAMES ${TRIPLE}-windres)

# Where to look for the target's libraries/headers
set(CMAKE_FIND_ROOT_PATH /usr/${TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
    echo "Generated toolchain-mingw64.cmake"
fi

# Try to locate a Qt installation under the mingw sysroot.
# Adjust QT_PREFIX if your Qt is installed elsewhere.
QT_PREFIX="/usr/x86_64-w64-mingw32"
if [[ -d "/usr/lib/mingw-w64/qt5" ]]; then
    QT_PREFIX="/usr/lib/mingw-w64/qt5"
fi

mkdir -p "${BUILD_DIR}"
cmake \
    -S "${SCRIPT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_PREFIX_PATH="${QT_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo ""
echo "Build complete.  DLL is at: ${BUILD_DIR}/devmenu_qt.dll"
echo "Deploy:"
echo "  cp ${BUILD_DIR}/devmenu_qt.dll \\"
echo "     ~/.local/share/Steam/steamapps/common/Spellbreak/Mods/dlls/"
