@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Makes build\release\Prepare-to-Coop-v<version>-PTDE.zip from a finished build.bat.
setlocal
cd /d "%~dp0"
for /f "tokens=3 delims= " %%v in ('findstr /c:"#define PREPARE_TO_COOP_VERSION" src\version.h') do set VERSION=%%~v
if "%VERSION%"=="" (
    echo Could not read the version from src\version.h
    exit /b 1
)
if not exist build\xinput1_3.dll (
    echo Run build.bat first.
    exit /b 1
)
if exist build\release rmdir /s /q build\release
mkdir build\release\files
copy /y build\xinput1_3.dll build\release\files >nul
copy /y build\PTDE-FPS-Unlock.ini build\release\files >nul
copy /y build\dxvk_d3d9.dll build\release\files >nul
copy /y README.md build\release\files >nul
copy /y NOTICE.md build\release\files >nul
copy /y LICENSE build\release\files\LICENSE.txt >nul
copy /y fps-unlock\LICENSE build\release\files\FPS-Unlock-LICENSE.txt >nul
copy /y fps-unlock\README.md build\release\files\FPS-Unlock-README.md >nul
copy /y fps-unlock\third_party\dxvk\LICENSE build\release\files\DXVK-LICENSE.txt >nul
powershell -NoProfile -Command "Compress-Archive -Path build\release\files\* -DestinationPath build\release\Prepare-to-Coop-v%VERSION%-PTDE.zip"
echo Made build\release\Prepare-to-Coop-v%VERSION%-PTDE.zip
