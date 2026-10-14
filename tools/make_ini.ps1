# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the shipped PTDE-FPS-Unlock.ini: Prepare to Co-op's section (PrepareToCoop.ini) followed by the FPS
# Unlock's own INI (fps-unlock\PTDE-FPS-Unlock.ini), with both versions filled in. Run from the repository root.
param([string]$Out = "build\PTDE-FPS-Unlock.ini")
$ErrorActionPreference = 'Stop'

$coop = [regex]::Match((Get-Content -Raw 'src\version.h'), '#define\s+PREPARE_TO_COOP_VERSION\s+"([^"]+)"').Groups[1].Value
$fps = [regex]::Match((Get-Content -Raw 'fps-unlock\src\ptde\dllmain.cpp'), 'ptde_fps_unlock_version\(\)\s*\{\s*return\s+"([^"]+)"').Groups[1].Value
if (-not $coop -or -not $fps) { throw "could not read the versions (Prepare to Co-op '$coop', FPS Unlock '$fps')" }

$head = (Get-Content -Raw 'PrepareToCoop.ini').Replace('@COOP@', $coop).Replace('@FPS@', $fps)
$tail = Get-Content -Raw 'fps-unlock\PTDE-FPS-Unlock.ini'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllText((Join-Path (Get-Location) $Out), $head + $tail, (New-Object Text.UTF8Encoding $false))
Write-Host "Wrote $Out (Prepare to Co-op $coop, FPS Unlock $fps)"
