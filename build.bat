@echo off
rem ---------------------------------------------------------------------------
rem build.bat -- build dd4win.exe (native 64-bit Windows, static) with the
rem WinLibs MinGW-w64 gcc, from cmd.exe or PowerShell: .\build.bat
rem
rem SPDX-License-Identifier: MIT OR GPL-2.0-or-later
rem Copyright (c) 2026 B. C. Services
rem
rem The only tool needed is gcc on PATH (the WinLibs MinGW-w64 build, UCRT
rem runtime).  The one source is named relative to this script's own folder,
rem so it runs from anywhere, and the executable is written beside it.
rem
rem The compile uses the strict flag set every C source of ours is held to --
rem C99 with no compiler extensions, every warning class the rules name, and
rem the build must produce NO warning at all.  Verify a build by the exe's
rem timestamp, never by this script's last line.
rem ---------------------------------------------------------------------------
setlocal
pushd "%~dp0"

rem Static single exe; pure Win32, one translation unit, the strict flags.
gcc -O2 -std=c99 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wdeclaration-after-statement -Wvla -static -o dd4win.exe dd4win.c
if errorlevel 1 goto :fail

echo built: dd4win.exe
popd
exit /b 0

rem The compile failed: gcc has already said why; leave the tree as it is.
:fail
echo BUILD FAILED
popd
exit /b 1
