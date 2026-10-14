@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Remakes assets\PrepareToCoop-Title.wmv (the title movie with PREPARE TO CO-OP EDITION) from the game's
rem movie and assets\PrepareToCoop-Title.png. Run it after changing the PNG, then build.bat.
rem usage: tools\build_title_movie.bat "<game>\DATA\movWW\frpg_title.wmv" [--refit] [--keep <dir>]
rem Needs Python 3 with numpy, scipy, Pillow and imageio-ffmpeg (or ffmpeg on PATH, or --ffmpeg <exe>).
setlocal
cd /d "%~dp0.."
if "%~1"=="" (
    echo usage: tools\build_title_movie.bat "<game>\DATA\movWW\frpg_title.wmv" [--refit] [--keep ^<dir^>]
    exit /b 2
)
if not exist build\tools mkdir build\tools

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

cmd /c "call %VCVARS% x86 >nul && cl /nologo /O2 /EHsc /W3 tools\wmvenc.cpp /Fobuild\tools\ /Febuild\tools\wmvenc.exe"
if errorlevel 1 exit /b 1

python tools\title_movie.py --movie "%~1" --encoder build\tools\wmvenc.exe %2 %3 %4 %5
if errorlevel 1 exit /b 1
