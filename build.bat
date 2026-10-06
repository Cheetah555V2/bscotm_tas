@echo off
setlocal enabledelayedexpansion
rem Builds build\bscotm_tas.exe and build\bscotm_hook.dll.
rem The game is 32-bit, so this needs a 32-bit (i686) MinGW-w64 toolchain.
rem Toolchain lookup order: %MINGW32% (its bin folder) > g++ on PATH > usual MSYS2 folders.

set "TC="
if defined MINGW32 if exist "%MINGW32%\g++.exe" set "TC=%MINGW32%"

if not defined TC (
    for /f "delims=" %%G in ('where g++ 2^>nul') do (
        if not defined TC (
            "%%G" -dumpmachine 2>nul | findstr /b i686 >nul && set "TC=%%~dpG"
        )
    )
)

if not defined TC (
    for %%R in ("%SystemDrive%\msys64" "C:\msys64" "D:\msys64" "%USERPROFILE%\scoop\apps\msys2\current" "C:\tools\msys64") do (
        if not defined TC if exist "%%~R\mingw32\bin\g++.exe" set "TC=%%~R\mingw32\bin"
    )
)

if not defined TC (
    echo Could not find a 32-bit MinGW-w64 toolchain ^(g++ for i686^).
    echo.
    echo Install MSYS2 from https://www.msys2.org, then in an MSYS2 shell run:
    echo     pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-make
    echo.
    echo If it is installed somewhere unusual, set MINGW32 to its bin folder, e.g.
    echo     set MINGW32=D:\tools\msys64\mingw32\bin
    exit /b 1
)

if "!TC:~-1!"=="\" set "TC=!TC:~0,-1!"
echo Using toolchain: !TC!
set "PATH=!TC!;%PATH%"

rem -B makes g++ use as/ld from the same bin folder; the copies in the toolchain's
rem i686-w64-mingw32\bin sub-folder cannot find their DLLs on some setups.
set "BDIR=!TC:\=/!"
if exist "!TC!\mingw32-make.exe" (set "MAKE=mingw32-make") else (set "MAKE=make")
%MAKE% "CXX=g++ -B!BDIR!/" %*
exit /b %errorlevel%
