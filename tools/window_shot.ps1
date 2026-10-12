# Screenshot of a test copy's own picture (even when it's behind other windows), at half size.
# Usage: window_shot.ps1 -Out <out.png> [-Id <process id>]
# The game saves its next frame itself (pipe command "shot", src/loader/d3d9.cpp). Reading the window from
# outside (PrintWindow) could leave the game not drawing any more, so that's not used.
param([string]$Out, [double]$Scale = 0.5, [int]$Id = 0)
Add-Type -AssemblyName System.Drawing
$game = if ($Id) { Get-Process -Id $Id -ErrorAction SilentlyContinue } else { Get-Process FGUY -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $game) { Write-Output "game not running"; exit 1 }
$Out = [System.IO.Path]::GetFullPath($Out)
$bmp = [System.IO.Path]::ChangeExtension($Out, ".tmp.bmp")
Remove-Item $bmp -ErrorAction SilentlyContinue
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$($game.Id)", [System.IO.Pipes.PipeDirection]::Out)
$pipe.Connect(3000)
$w = New-Object System.IO.StreamWriter($pipe)
$w.WriteLine("shot $bmp"); $w.Flush(); $w.Dispose()
$until = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $until -and -not (Test-Path $bmp)) { Start-Sleep -Milliseconds 100 }
if (-not (Test-Path $bmp)) { Write-Output "no frame within 5 s (the game isn't drawing)"; exit 1 }
Start-Sleep -Milliseconds 200   # (the file is written in one go; let it close)
$img = [System.Drawing.Image]::FromFile($bmp)
$img.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$img.Dispose()
Remove-Item $bmp -ErrorAction SilentlyContinue
