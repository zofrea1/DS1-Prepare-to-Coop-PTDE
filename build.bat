@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Builds Prepare to Co-op Edition: one xinput1_3.dll with the DS1 PTDE FPS Unlock (the fps-unlock submodule)
rem and the co-op, the shipped PTDE-FPS-Unlock.ini, and DXVK's dxvk_d3d9.dll (the FPS Unlock's optional DXVK).
setlocal
cd /d "%~dp0"
if not exist fps-unlock\src\ptde\sources.txt (
    echo The FPS Unlock is missing. Run: git submodule update --init
    exit /b 1
)
if not exist build\obj mkdir build\obj

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

rem The FPS Unlock's sources (its own list, so new files there need nothing here), then ours. Ours replace its
rem extension_none.cpp with src\extension.cpp.
set RSP=build\sources.rsp
> %RSP% (for /f "usebackq eol=# delims=" %%f in ("fps-unlock\src\ptde\sources.txt") do @echo fps-unlock\%%f)
>> %RSP% (for /f "usebackq eol=# delims=" %%f in ("src\sources.txt") do @echo %%f)

cmd /c "call %VCVARS% x86 && rc /nologo /fo build\obj\coop.res src\coop.rc && cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /Ifps-unlock\src\common /Ifps-unlock\src\ptde /Isrc @%RSP% build\obj\coop.res /Fobuild\obj\ /Febuild\xinput1_3.dll /link /DEF:fps-unlock\src\ptde\xinput1_3.def /OPT:REF"
if errorlevel 1 exit /b 1

set PSModulePath=
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_ini.ps1 -Out build\PTDE-FPS-Unlock.ini
if errorlevel 1 exit /b 1
rem DXVK 3.1.1's 32-bit d3d9.dll, pinned and SHA-256 checked by the FPS Unlock's script.
powershell -NoProfile -ExecutionPolicy Bypass -File fps-unlock\tools\fetch_dxvk.ps1 -OutDir build
if errorlevel 1 exit /b 1

echo Built build\xinput1_3.dll, build\PTDE-FPS-Unlock.ini and build\dxvk_d3d9.dll
