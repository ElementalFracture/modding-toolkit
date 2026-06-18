cmake_minimum_required(VERSION 3.20)
project(devmenu_imgui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# ImGui — fetched at configure time.  Pin to a specific tag for reproducibility.
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ---------------------------------------------------------------------------
# DLL target
# ---------------------------------------------------------------------------
add_library(devmenu_imgui SHARED
    devmenu_imgui.cpp

    # ImGui core
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp

    # Win32 + D3D11 backends
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(devmenu_imgui PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(devmenu_imgui PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
)

# Statically embed the MinGW C/C++ runtimes so the DLL has no external
# libgcc_s_seh-1.dll / libstdc++-6.dll dependencies on the target Windows host.
target_link_options(devmenu_imgui PRIVATE -static-libgcc -static-libstdc++)

# mingw adds a "lib" prefix by default — strip it so the DLL name is exact.
set_target_properties(devmenu_imgui PROPERTIES PREFIX "")

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS devmenu_imgui
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
)
