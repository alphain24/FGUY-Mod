# Presses buttons on the loader's virtual controller (needs [Debug] TestInput=1 in FGUYMod.ini).
#   pad.ps1 down down a          -> pad 0: d-pad down twice, then A
#   pad.ps1 -Pad 1 start         -> pad 1: Start
# Buttons: a b x y start back up down left right lb rb
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Buttons,
    [int]$Pad = 0
)
$game = Get-Process FGUY -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) { Write-Output "game not running"; exit 1 }
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$($game.Id)", [System.IO.Pipes.PipeDirection]::Out)
$pipe.Connect(3000)
$w = New-Object System.IO.StreamWriter($pipe)
$w.WriteLine("press $Pad $($Buttons -join ' ')")
$w.Flush(); $w.Dispose()
# Wait for the presses to play out (300 ms each) before returning.
Start-Sleep -Milliseconds (300 * $Buttons.Count + 200)
Write-Output "pressed: $($Buttons -join ' ')"
