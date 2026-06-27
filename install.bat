@echo off
:: Re-launch as admin if not already elevated.
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
setlocal EnableDelayedExpansion
title Elemental Fracture - Mod Installer

:: ==================================================================
::  CONFIGURATION
:: ==================================================================
set "MOD_ZIP_URL=https://cdn.elefrac.com/patch/mod/mod.zip"
set "AUTH_PORTAL=https://elefrac.com"

set "LOADER_DLL=xinput1_3.dll"
set "MOD_DLLS=auth_injector.dll devmenu_imgui.dll qt_devmenu.dll"

:: ==================================================================
::  GAME ROOT  --  this script lives in the Spellbreak folder
:: ==================================================================
set "GAME_PATH=%~dp0"
if "!GAME_PATH:~-1!"=="\" set "GAME_PATH=!GAME_PATH:~0,-1!"
set "TMP_DIR=!GAME_PATH!\_modtmp"

if not exist "!GAME_PATH!\g3\Binaries\Win64\" (
    cls
    echo.
    echo  [!]  This script must be placed in the root Spellbreak game folder.
    echo       Expected to find:  g3\Binaries\Win64\
    echo.
    echo       Copy install.bat into the same folder that contains
    echo       the "g3" and "Engine" folders, then run it again.
    echo.
    pause
    exit /b 1
)

:: ==================================================================
::  MAIN MENU
:: ==================================================================
:MENU
cls
echo.
echo  +--------------------------------------------------+
echo  ^|    Elemental Fracture  ^|  Mod Installer          ^|
echo  +--------------------------------------------------+
echo.
echo    Game  :  !GAME_PATH!
echo.
echo    [1]  Install / Update mods
echo    [2]  Uninstall mods
echo    [3]  Set auth token
echo    [4]  Verify installation
echo    [0]  Exit
echo.
set "CHOICE="
set /p "CHOICE=   Choice: "

if "!CHOICE!"=="1" goto :INSTALL
if "!CHOICE!"=="2" goto :UNINSTALL
if "!CHOICE!"=="3" goto :SET_TOKEN
if "!CHOICE!"=="4" goto :VERIFY
if "!CHOICE!"=="0" goto :BYE
goto :MENU

:: ==================================================================
::  INSTALL / UPDATE
:: ==================================================================
:INSTALL
cls
echo.
echo  -- Install / Update ---------------------------------------
echo.
echo  Downloading mod package...
echo.

if exist "!TMP_DIR!" rmdir /s /q "!TMP_DIR!"
mkdir "!TMP_DIR!"

call :DOWNLOAD "!MOD_ZIP_URL!" "!TMP_DIR!\mod.zip"
if !errorlevel! neq 0 (
    echo.
    echo  [!]  Download failed. Check your connection and try again.
    echo.
    rmdir /s /q "!TMP_DIR!" 2>nul
    pause
    goto :MENU
)

echo  Extracting...
mkdir "!TMP_DIR!\pkg"
call :EXTRACT "!TMP_DIR!\mod.zip" "!TMP_DIR!\pkg"
if !errorlevel! neq 0 (
    echo.
    echo  [!]  Extraction failed.
    echo.
    rmdir /s /q "!TMP_DIR!" 2>nul
    pause
    goto :MENU
)

echo  Package contents:
dir /b "!TMP_DIR!\pkg"
echo.

mkdir "!GAME_PATH!\Mods\dlls"     2>nul
mkdir "!GAME_PATH!\Mods\commands" 2>nul

echo  Installing files...
echo.

set "_pkg=!TMP_DIR!\pkg"

if exist "!_pkg!\!LOADER_DLL!" (
    copy /Y "!_pkg!\!LOADER_DLL!" "!GAME_PATH!\g3\Binaries\Win64\xinput1_3.dll" >nul 2>&1
    if !errorlevel! equ 0 (echo    [OK]       xinput1_3.dll) else (echo    [ERROR]    xinput1_3.dll - is the game running?)
) else (
    echo    [MISSING]  xinput1_3.dll - not in package
)

for %%F in (%MOD_DLLS%) do (
    if exist "!_pkg!\%%F" (
        copy /Y "!_pkg!\%%F" "!GAME_PATH!\Mods\dlls\%%F" >nul 2>&1
        if !errorlevel! equ 0 (echo    [OK]       %%F) else (echo    [ERROR]    %%F - is the game running?)
    ) else (
        echo    [MISSING]  %%F - not in package
    )
)

rmdir /s /q "!TMP_DIR!" 2>nul

:: Remove the internet Zone.Identifier mark so Windows loads the DLLs without restriction.
powershell -NoProfile -Command ^
  "Get-ChildItem '!GAME_PATH!\Mods\dlls\' -Filter '*.dll' | Unblock-File; Unblock-File '!GAME_PATH!\g3\Binaries\Win64\xinput1_3.dll'" >nul 2>&1

echo.
echo  Done. Launch Spellbreak and press F8 in-game to open the menu.
echo.
pause
goto :MENU

:: ==================================================================
::  UNINSTALL
:: ==================================================================
:UNINSTALL
cls
echo.
echo  -- Uninstall -----------------------------------------------
echo.
echo  This removes the mod loader and all mod DLLs.
echo  Your auth token will NOT be touched.
echo.
set "CONFIRM="
set /p "CONFIRM=  Type YES to confirm: "
if /i not "!CONFIRM!"=="YES" (
    echo.
    echo  Cancelled.
    timeout /t 2 >nul
    goto :MENU
)
echo.

call :REMOVE "!GAME_PATH!\g3\Binaries\Win64\xinput1_3.dll" "Mod loader (xinput1_3.dll)"
for %%F in (!MOD_DLLS!) do (
    call :REMOVE "!GAME_PATH!\Mods\dlls\%%F" "%%F"
)

echo.
echo  Uninstall complete.
echo.
pause
goto :MENU

:: ==================================================================
::  SET AUTH TOKEN
:: ==================================================================
:SET_TOKEN
cls
echo.
echo  -- Auth Token Setup ----------------------------------------
echo.

set "_tok_file=!GAME_PATH!\Mods\commands\auth_token.txt"
mkdir "!GAME_PATH!\Mods\commands" 2>nul

if exist "!_tok_file!" (
    set "_cur="
    set /p "_cur="<"!_tok_file!"
    if defined _cur (
        echo   Current token:  !_cur:~0,8!...
    ) else (
        echo   Token file present but empty.
    )
) else (
    echo   No token installed yet.
)

echo.
echo   Tokens are issued by a server admin or via:
echo     !AUTH_PORTAL!
echo.
echo   Paste your token and press Enter.  Leave blank to cancel.
echo.
set "_new_tok="
set /p "_new_tok=  Token: "

if "!_new_tok!"=="" (
    echo.
    echo  Cancelled.
    timeout /t 2 >nul
    goto :MENU
)

>"!_tok_file!" echo !_new_tok!

echo.
echo   Token saved to:
echo     !_tok_file!
echo.
echo   Reconnect to the server for it to take effect.
echo.
pause
goto :MENU

:: ==================================================================
::  VERIFY
:: ==================================================================
:VERIFY
cls
echo.
echo  -- Verify Installation -------------------------------------
echo.

set "_ok=0"
set "_miss=0"

call :CHK "!GAME_PATH!\g3\Binaries\Win64\xinput1_3.dll"    "Mod loader      (xinput1_3.dll)"
for %%F in (!MOD_DLLS!) do (
    call :CHK "!GAME_PATH!\Mods\dlls\%%F"                  "%%F"
)
call :CHK "!GAME_PATH!\Mods\commands\auth_token.txt"        "Auth token"

echo.
echo  ------------------------------------------------------------
if !_miss! equ 0 (
    echo   All files present.  You are good to go.
) else (
    echo   !_miss! file^(s^) missing -- run option 1 to install.
)
echo.
pause
goto :MENU

:: ==================================================================
::  EXIT
:: ==================================================================
:BYE
echo.
echo  Good luck out there.
echo.
exit /b 0

:: ==================================================================
::  SUBROUTINES
:: ==================================================================

:DOWNLOAD
set "_url=%~1"
set "_out=%~2"
curl --version >nul 2>&1
if !errorlevel! equ 0 (
    curl -sSL --fail "!_url!" -o "!_out!"
) else (
    powershell -NoProfile -Command "try{Invoke-WebRequest '!_url!' -OutFile '!_out!' -UseBasicParsing -EA Stop}catch{exit 1}"
)
if !errorlevel! neq 0 exit /b 1

:: Sanity-check: a valid zip must be at least 22 bytes (empty zip).
:: If the server returned an HTML error page curl still exits 0.
set "_sz=0"
for %%I in ("%~2") do set "_sz=%%~zI"
if !_sz! lss 1024 (
    echo  [!]  Downloaded file is too small ^(!_sz! bytes^) -- CDN may have returned an error.
    exit /b 1
)
exit /b 0

:EXTRACT
tar -xf "%~1" -C "%~2" >nul 2>&1
if !errorlevel! equ 0 exit /b 0
powershell -NoProfile -Command ^
  "try { Expand-Archive -LiteralPath '%~1' -DestinationPath '%~2' -Force -ErrorAction Stop; exit 0 } catch { Write-Error $_.Exception.Message; exit 1 }"
if !errorlevel! equ 0 exit /b 0
exit /b 1


:REMOVE
set "_rlbl=%~2"
if exist "%~1" (
    del /f "%~1" >nul 2>&1
    if !errorlevel! equ 0 (
        echo    [Removed]   !_rlbl!
    ) else (
        echo    [Error]     !_rlbl!  - delete failed, is the game running?
    )
) else (
    echo    [Skipped]   !_rlbl!  - not present
)
exit /b 0

:CHK
set "_clbl=%~2"
if exist "%~1" (
    echo    [OK]      !_clbl!
    set /a _ok+=1
) else (
    echo    [MISSING] !_clbl!
    set /a _miss+=1
)
exit /b 0
