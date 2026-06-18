cmake_minimum_required(VERSION 3.16)
project(devmenu_qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Qt — find whatever version is available in the cross-compile environment.
# The mingw64 Qt package in the Humble-ROS2 Distrobox ships Qt 5; set
# CMAKE_PREFIX_PATH if CMake can't locate it automatically, e.g.:
#   cmake -DCMAKE_PREFIX_PATH=/usr/x86_64-w64-mingw32/qt5 ...
# ---------------------------------------------------------------------------
find_package(Qt5 COMPONENTS Core Widgets QUIET)
if (NOT Qt5_FOUND)
    find_package(Qt6 COMPONENTS Core Widgets REQUIRED)
    message(STATUS "Using Qt6")
else()
    message(STATUS "Using Qt5")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(devmenu_qt SHARED
    devmenu_qt.cpp
    DevMenuWindow.cpp
    DevMenuWindow.h
    devmenu_qt.h
)

if (Qt5_FOUND)
    target_link_libraries(devmenu_qt PRIVATE Qt5::Core Qt5::Widgets)
else()
    target_link_libraries(devmenu_qt PRIVATE Qt6::Core Qt6::Widgets)
endif()

# Strip the "lib" prefix that mingw adds by default.
set_target_properties(devmenu_qt PROPERTIES PREFIX "")

# ---------------------------------------------------------------------------
# Install — copy the DLL to the game's Mods/dlls/ drop folder.
# Adjust DESTDIR on the cmake --install invocation to override the path.
# ---------------------------------------------------------------------------
install(TARGETS devmenu_qt
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
)
