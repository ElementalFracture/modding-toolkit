#!/usr/bin/env bash
# Build devmenu_imgui.dll using the x86_64-w64-mingw32 cross-compiler.
# ImGui is fetched automatically by CMake FetchContent on first run.
#
# Usage (from this directory):
#   bash build_mingw.sh
#
# Output: ./build/devmenu_imgui.dll

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

TOOLCHAIN_FILE="${SCRIPT_DIR}/toolchain-mingw64.cmake"

if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
    cat > "${TOOLCHAIN_FILE}" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TRIPLE x86_64-w64-mingw32)

find_program(CMAKE_C_COMPILER   NAMES ${TRIPLE}-gcc   REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${TRIPLE}-g++   REQUIRED)
find_program(CMAKE_RC_COMPILER  NAMES ${TRIPLE}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
    echo "Generated toolchain-mingw64.cmake"
fi

mkdir -p "${BUILD_DIR}"

cmake \
    -S "${SCRIPT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo ""
echo "Build complete: ${BUILD_DIR}/devmenu_imgui.dll"
echo "Deploy:"
echo "  cp ${BUILD_DIR}/devmenu_imgui.dll \\"
echo "     ~/.local/share/Steam/steamapps/common/Spellbreak/Mods/dlls/"
