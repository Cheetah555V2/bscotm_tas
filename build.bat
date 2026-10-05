@echo off
rem Builds build\bscotm_tas.exe and build\bscotm_hook.dll with the 32-bit MinGW-w64 toolchain.
rem Set MINGW32 to its bin folder if it is not already on PATH.
if not defined MINGW32 set MINGW32=D:\Code_file\MSYS2\mingw32\bin
set PATH=%MINGW32%;%PATH%
set BDIR=%MINGW32:\=/%
rem -B: as/ld in the toolchain sub-folder cannot find their DLLs on some setups.
mingw32-make "CXX=g++ -B%BDIR%/" %*
