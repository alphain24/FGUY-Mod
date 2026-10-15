# Builds the players' download:
#   build\release\FGUYMod-<version>-Setup.exe   the one file players run (src/installer/setup.cpp, with the zip in it)
#   build\release\FGUYMod-<version>.zip         what goes into the game folder, nothing else (for the updater and
#                                                 anyone installing by hand)
#   build\release\FGUYMod-<version>.zip.sha256  its checksum: the mod's updater installs a release only if it matches
# Upload all three to the GitHub release (src/loader/updater.cpp). The version is kModVersion in src/loader/online.cpp.
#   make_release.ps1 [-Server host:port]   (the lobby server players connect to; default: what FGUYMod.ini says)
# In it: dinput8.dll, FGUYMod\FGUYMod.ini, FGUYMod\dxvk (DXVK and its licence), FGUYMod\ui\*.fgpatch (only our
# changes to the game's menus), FGUYMod\sounds (the loading screen's music - a re-recording, from
# dist\FGUYMod\sounds: tools/make_sounds.py; not in the code repo), INSTALL.txt. Never in it: game files, the test
# bot's scripts, logs or saves.
param([string]$Server = "")
$P = Split-Path $PSScriptRoot -Parent
$dll = Join-Path $P "build\dinput8.dll"
if (-not (Test-Path $dll)) { Write-Output "build first: build.bat"; exit 1 }
$stage = Join-Path $P "build\release\stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\FGUYMod\ui", "$stage\FGUYMod\dxvk" | Out-Null
Copy-Item $dll $stage
Copy-Item "$P\dist\FGUYMod\FGUYMod.ini" "$stage\FGUYMod\"
Copy-Item "$P\dist\FGUYMod\dxvk\*" "$stage\FGUYMod\dxvk\"
Copy-Item "$P\dist\FGUYMod\ui\*.fgpatch" "$stage\FGUYMod\ui\"
if (Test-Path "$P\dist\FGUYMod\sounds\*.wav") {
    New-Item -ItemType Directory -Force "$stage\FGUYMod\sounds" | Out-Null
    Copy-Item "$P\dist\FGUYMod\sounds\*.wav" "$stage\FGUYMod\sounds\"
} else { Write-Output "(no loading-screen sounds in dist\FGUYMod\sounds - the release goes without)" }
if ($Server) {
    $ini = "$stage\FGUYMod\FGUYMod.ini"
    $text = Get-Content $ini -Raw
    if ($text -match '(?m)^Server=') { $text = $text -replace '(?m)^Server=.*$', "Server=$Server" }
    elseif ($text -match '(?m)^\[Online\]') { $text = $text -replace '(?m)^\[Online\]\s*$', "[Online]`r`nServer=$Server" }
    else { $text += "`r`n[Online]`r`nServer=$Server`r`n" }
    Set-Content $ini $text -NoNewline -Encoding ascii
}
@"
FGUY Mod - Family Guy: Back to the Multiverse (PC)
==================================================

You need your own copy of the game. This download has none of its files.

Install
  Easiest: run FGUYMod-Setup.exe - it finds the game and installs the mod (run it again to update or remove it).
  By hand: copy dinput8.dll and the FGUYMod folder from this zip into the game's folder (the one with FGUY.exe),
  then start the game as usual.

Play online
  Main Menu > ONLINE. Host a game or join one from the list; everyone readies up and the host starts.
  Left a match, or your game closed in it? While it's still on, its lobby shows REJOIN MATCH: your game
  catches up with the match (a minute or two - the loading screen shows how far) and you're back in.

Settings
  FGUYMod\FGUYMod.ini (open with Notepad): display mode, frame rate, online server.
  Most are in the game's own Options > Screen too.

Uninstall
  Run FGUYMod-Setup.exe and pick Remove - or delete dinput8.dll and the FGUYMod folder from the game's folder.

Mods
  Main Menu > MODS: separate mods, put in FGUYMod\mods\ - switch them on and off there (takes a restart).
  Online, everyone in a lobby needs the same gameplay mods.

Updates
  The ONLINE screen says when there's a newer version; click it to install, then restart the game.

Problems? If a match goes out of sync or the game crashes, a report is saved in FGUYMod\reports\
  (the ONLINE screen's Y / Reports opens the folder) - send it. The logs are in FGUYMod\FGUYMod_<number>.log.

DXVK (FGUYMod\dxvk) is by Philip Rebohle and contributors, zlib licence (FGUYMod\dxvk\LICENSE.txt).
"@ | Set-Content "$stage\INSTALL.txt" -Encoding ascii
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.ho', '.gfx', '.sav', '.log', '.bat' -or
    ($_.Extension -eq '.wav' -and $_.DirectoryName -ne "$stage\FGUYMod\sounds") }
if ($bad) { Write-Output "not releasing - these don't belong in it:"; $bad.FullName; exit 1 }
$version = [regex]::Match((Get-Content "$P\src\loader\online.cpp" -Raw), 'kModVersion = "([^"]+)"').Groups[1].Value
if (-not $version) { Write-Output "no kModVersion in src\loader\online.cpp"; exit 1 }
$zip = Join-Path $P "build\release\FGUYMod-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
(Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() + "  FGUYMod-$version.zip" | Set-Content "$zip.sha256" -Encoding ascii -NoNewline
Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
"-> $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB) and $zip.sha256"

# The setup: src/installer/setup.cpp with the zip (resource 1) and the version (resource 2) in it.
$rel = Join-Path $P "build\release"
$setup = Join-Path $rel "FGUYMod-$version-Setup.exe"
Set-Content (Join-Path $rel "version.txt") $version -Encoding ascii -NoNewline
@"
1 RCDATA "FGUYMod-$version.zip"
2 RCDATA "version.txt"
"@ | Set-Content (Join-Path $rel "setup.rc") -Encoding ascii
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
$src = Join-Path $P "src\installer\setup.cpp"
$cmd = "call `"$vs`" x86 >nul && cd /d `"$rel`" && rc /nologo /fo setup.res setup.rc && " +
       "cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE `"$src`" setup.res /Fo`"$rel\\`" /Fe`"$setup`" " +
       "/link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED"
cmd /c $cmd
if (-not (Test-Path $setup)) { Write-Output "the setup didn't build"; exit 1 }
"-> $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
