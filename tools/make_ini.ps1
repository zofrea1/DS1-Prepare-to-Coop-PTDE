# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the shipped PTDE-Coop-FPS-Unlock.ini from Prepare to Co-op's template (PrepareToCoop.ini) and the FPS Unlock's
# own INI (fps-unlock\PTDE-FPS-Unlock.ini), with both versions filled in. Run from the repository root.
#
# Both files put the settings a player is likely to change first and the rest after a line. The template has two
# places for the FPS Unlock: @FPS_MAIN@ takes its first part (from its first section to its line: frame rate and
# display), @FPS_REST@ the rest (after its line: the fixes, the speed-ups and [Diagnostics], which stays last). The
# FPS Unlock's own heading and line are left out: the template has its own.
param([string]$Out = "build\PTDE-Coop-FPS-Unlock.ini")
$ErrorActionPreference = 'Stop'

$coop = [regex]::Match((Get-Content -Raw 'src\version.h'), '#define\s+PREPARE_TO_COOP_VERSION\s+"([^"]+)"').Groups[1].Value
$fps = [regex]::Match((Get-Content -Raw 'fps-unlock\src\ptde\dllmain.cpp'), 'ptde_fps_unlock_version\(\)\s*\{\s*return\s+"([^"]+)"').Groups[1].Value
if (-not $coop -or -not $fps) { throw "could not read the versions (Prepare to Co-op '$coop', FPS Unlock '$fps')" }

$head = (Get-Content -Raw 'PrepareToCoop.ini').Replace("`r`n", "`n").Replace('@COOP@', $coop).Replace('@FPS@', $fps)
$tail = (Get-Content -Raw 'fps-unlock\PTDE-FPS-Unlock.ini').Replace("`r`n", "`n")

$first = [regex]::Match($tail, '(?m)^\[')
$line = [regex]::Match($tail, '(?ms)^# =+\n# Fixes and speed-ups: leave these as they are\..*?^# =+\n')
if (-not $first.Success -or -not $line.Success -or $line.Index -lt $first.Index) {
    throw "fps-unlock\PTDE-FPS-Unlock.ini: no sections, or no '# Fixes and speed-ups: leave these as they are.' line"
}
if (-not $head.Contains('@FPS_MAIN@') -or -not $head.Contains('@FPS_REST@')) {
    throw "PrepareToCoop.ini needs both @FPS_MAIN@ and @FPS_REST@"
}
$main = $tail.Substring($first.Index, $line.Index - $first.Index).TrimEnd() + "`n"
$rest = $tail.Substring($line.Index + $line.Length).TrimStart("`n")
$text = $head.Replace("@FPS_MAIN@`n", $main + "`n").Replace("@FPS_MAIN@", $main).Replace('@FPS_REST@', $rest.TrimEnd() + "`n")
$text = $text.TrimEnd() + "`n"
if (-not [regex]::IsMatch($text, '(?m)^\[Diagnostics\]') -or
    $text.LastIndexOf('[Diagnostics]') -lt $text.LastIndexOf('[PrepareToCoopDetails]')) {
    throw "the written INI must end with the FPS Unlock's [Diagnostics]"
}
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllText((Join-Path (Get-Location) $Out), $text, (New-Object Text.UTF8Encoding $false))
Write-Host "Wrote $Out (Prepare to Co-op $coop, FPS Unlock $fps)"
