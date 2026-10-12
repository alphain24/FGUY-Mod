# Everyone opens the online pause menu -> the game pauses for real and its own pause menu shows - online only
# RESUME and QUIT GAME (the pause patch). Its RESUME unpauses everyone; the match stays in sync.
#   test_pause_all.ps1        (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse", [switch]$Click)
$P = Split-Path $PSScriptRoot -Parent
$shots = Join-Path $P "build\shots"
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Count([int]$id, [string]$text) { if (-not (Test-Path (Log $id))) { return 0 }; @(Select-String -Path (Log $id) -Pattern $text -SimpleMatch).Count }
function Has([int]$id, [string]$text) { (Count $id $text) -gt 0 }
function WaitFor([int]$id, [string]$text, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if (Has $id $text) { return $true }; Start-Sleep 1 }
    return $false
}
function Step([string]$what, [bool]$ok) { if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 } }
function Keys([int]$id, [string[]]$steps) { & "$PSScriptRoot\kbm_test.ps1" -Id $id @steps | Out-Null }

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
& "$PSScriptRoot\wait_gpu.ps1"
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Controls keyboard,controller
$out | Where-Object { $_ -match "^lobby copies: host (\d+), guest (\d+)" } | Out-Null
$hOwn = [int]$Matches[1]; $cOwn = [int]$Matches[2]
$sess = @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ } |
          Where-Object { $_ -ne $hOwn -and $_ -ne $cOwn -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -Last 1
$c = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -Last 1
Step "the match runs (host $h, guest $c)" ([bool]($h -and $c))
Keys $h @("cursor 300 300", "key 27 150", "wait 500")
& "$PSScriptRoot\pad.ps1" -Id $c start | Out-Null
Step "everyone's menu open: the game pauses" ((WaitFor $h "pausing the game" 5) -and (WaitFor $c "pausing the game" 5))
Start-Sleep 3
& "$PSScriptRoot\window_shot.ps1" -Id $h -Out (Join-Path $shots "pause_all_host.png")
& "$PSScriptRoot\window_shot.ps1" -Id $c -Out (Join-Path $shots "pause_all_guest.png")
if ($Click) {   # a mouse click on RESUME instead of Enter: the game's pointer there (a mouse move sent to that
    # window only - the real mouse stays put), the synced cursor too, then the click
    Add-Type -Name W -Namespace T -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
public struct RECT { public int L, T, R, B; }
'@
    $wnd = (Get-Process -Id $h).MainWindowHandle
    $rc = New-Object T.W+RECT
    [T.W]::GetClientRect($wnd, [ref]$rc) | Out-Null
    $x = [int](($rc.R - $rc.L) * 8200 / 32767); $y = [int](($rc.B - $rc.T) * 7900 / 32767)
    [T.W]::PostMessage($wnd, 0x200, [IntPtr]::Zero, [IntPtr](($y -shl 16) -bor $x)) | Out-Null   # WM_MOUSEMOVE
    Keys $h @("cursor 8200 7900", "wait 400")
    & "$PSScriptRoot\window_shot.ps1" -Id $h -Out (Join-Path $shots "pause_all_hover.png")
    Keys $h @("click 150", "wait 1500")
} else {
    Keys $h @("key 13 150", "wait 1500")   # RESUME (the first button; the host's keyboard drives this menu)
}
Step "its RESUME unpauses everyone" ((WaitFor $h "Pause: the game resumed" 5) -and (WaitFor $c "Pause: the game resumed" 5))
$before = (Count $h "DESYNC") + (Count $c "DESYNC")
Start-Sleep 15
Step "and the match stays in sync" ((Count $h "DESYNC") + (Count $c "DESYNC") -eq $before)
