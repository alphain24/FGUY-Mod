# Builds the players' download: build\release\FGUYMod-<version>.zip - what goes into the game folder, nothing else - and
# FGUYMod-<version>.zip.sha256 (its checksum: the mod's updater installs a release only if it matches; upload both to the
# GitHub release, src/loader/updater.cpp). The version is kModVersion in src/loader/online.cpp.
#   make_release.ps1 [-Server host:port]   (the lobby server players connect to; default: what FGUYMod.ini says)
# In it: dinput8.dll, FGUYMod\FGUYMod.ini, FGUYMod\dxvk (DXVK and its licence), FGUYMod\ui\*.fgpatch (only our
# changes to the game's menus), INSTALL.txt. Never in it: game files, the show's sounds (FGUYMod\sounds), the
# test bot's scripts, logs or saves.
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
  1. Open the game's folder (the one with FGUY.exe).
  2. Copy dinput8.dll and the FGUYMod folder from this zip into it.
  3. Start the game as usual.

Play online
  Main Menu > ONLINE. Host a game or join one from the list; everyone readies up and the host starts.
  Left a match, or your game closed in it? While it's still on, its lobby shows REJOIN MATCH: your game
  catches up with the match (a minute or two - the loading screen shows how far) and you're back in.

Settings
  FGUYMod\FGUYMod.ini (open with Notepad): display mode, frame rate, online server.
  Most are in the game's own Options > Screen too.

Uninstall
  Delete dinput8.dll and the FGUYMod folder from the game's folder.

Mods
  Main Menu > MODS: separate mods, put in FGUYMod\mods\ - switch them on and off there (takes a restart).
  Online, everyone in a lobby needs the same gameplay mods.

Updates
  The ONLINE screen says when there's a newer version; click it to install, then restart the game.

Problems? If a match goes out of sync or the game crashes, a report is saved in FGUYMod\reports\
  (the ONLINE screen's Y / Reports opens the folder) - send it. The logs are in FGUYMod\FGUYMod_<number>.log.

DXVK (FGUYMod\dxvk) is by Philip Rebohle and contributors, zlib licence (FGUYMod\dxvk\LICENSE.txt).
"@ | Set-Content "$stage\INSTALL.txt" -Encoding ascii
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.wav', '.ho', '.gfx', '.sav', '.log', '.bat' }
if ($bad) { Write-Output "not releasing - these don't belong in it:"; $bad.FullName; exit 1 }
$version = [regex]::Match((Get-Content "$P\src\loader\online.cpp" -Raw), 'kModVersion = "([^"]+)"').Groups[1].Value
if (-not $version) { Write-Output "no kModVersion in src\loader\online.cpp"; exit 1 }
$zip = Join-Path $P "build\release\FGUYMod-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
(Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() + "  FGUYMod-$version.zip" | Set-Content "$zip.sha256" -Encoding ascii -NoNewline
Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
"-> $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB) and $zip.sha256"
