# Test tool: pretend keyboard / mouse input for an online keyboard player (through the loader's test pipe,
# so it works in the background and doesn't touch the real keyboard). One command per argument:
#   kbm_test.ps1 -Id 1234 "key 87 2000" "mouse 20 0 30" "click 300" "wait 1000"
#     key <Windows key code> <ms>     hold a key (W = 87, A = 65, S = 83, D = 68, Space = 32, R = 82, ...)
#     mouse <dx> <dy> <ticks>         move the mouse by dx, dy every tick (1/60 s) for that many ticks
#     click <ms>                      hold the left mouse button
#     wait <ms>
param([int]$Id, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Commands)
foreach ($c in $Commands) {
    if ($c -match '^wait (\d+)') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$Id", [System.IO.Pipes.PipeDirection]::Out)
    $pipe.Connect(3000)
    $w = New-Object System.IO.StreamWriter($pipe)
    $w.WriteLine("kbm $c")
    $w.Flush(); $w.Dispose()
    if ($c -match '^(key \d+|click) (\d+)') { Start-Sleep -Milliseconds ([int]$Matches[2]) }
    elseif ($c -match '^mouse -?\d+ -?\d+ (\d+)') { Start-Sleep -Milliseconds ([int]$Matches[1] * 17) }
}
Write-Output "sent: $($Commands -join ', ')"
