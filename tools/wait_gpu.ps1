# Waits until Direct3D 9 devices can be created again (after test copies close, the graphics driver can
# refuse new devices for a while). Prints how long it took. Usage: wait_gpu.ps1 [-Seconds 240]
param([int]$Seconds = 240)
$test = Join-Path $PSScriptRoot "d3d9test\d3d9test.exe"
$start = Get-Date
while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
    $out = & $test
    if (($out | Select-String "^game: .* -> 0$") -and -not ($out | Select-String "-> 0x")) {
        "graphics ready after {0:N0} s" -f ((Get-Date) - $start).TotalSeconds
        return
    }
    Start-Sleep 5
}
Write-Warning "the graphics driver still refuses new devices after $Seconds s"
