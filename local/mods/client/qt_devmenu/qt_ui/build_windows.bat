@echo off
:: Build devmenu_qt.dll on Windows with a standard Qt installation.
::
:: Prerequisites:
::   - Qt 5 or Qt 6 for MSVC or MinGW, installed via the Qt installer
::   - CMake on PATH
::   - A matching C++ compiler (MSVC via Developer Command Prompt, or MinGW on PATH)
::
:: Usage:
::   Open a Qt-enabled terminal (e.g. the Qt Creator "Qt 6.x.x MSVC" shortcut
::   or a Developer Command Prompt with Qt on PATH), then run:
::
::     build_windows.bat
::
:: The resulting devmenu_qt.dll will be in .\build\Release\devmenu_qt.dll.
:: Copy it to:
::   %LOCALAPPDATA%\Steam\steamapps\common\Spellbreak\Mods\dlls\devmenu_qt.dll

setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo CMake configure failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo Build complete.  DLL is at:
echo   %BUILD_DIR%\Release\devmenu_qt.dll
echo.
echo Deploy to Spellbreak:
echo   copy "%BUILD_DIR%\Release\devmenu_qt.dll" ^
echo        "%LOCALAPPDATA%\Steam\steamapps\common\Spellbreak\Mods\dlls\"
