# Closes only the game copies that test scripts started (their process ids are recorded in
# <game>\FGUYMod\test_pids.txt), never a copy you started yourself.
# Closes them the normal way (like clicking the window's X) and waits: force-killing a copy leaves
# the graphics driver unable to create Direct3D 9 devices for a minute or two on some PCs.
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse")
$file = Join-Path $GameDir "FGUYMod\test_pids.txt"
if (-not (Test-Path $file)) { return }
$mine = @()
foreach ($id in Get-Content $file) {
    if (-not $id) { continue }
    $p = Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "FGUY") {
        try {  # ask the loader to exit cleanly (test copies have the test-input pipe)
            $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$($p.Id)", [System.IO.Pipes.PipeDirection]::Out)
            $pipe.Connect(2000)
            $w = New-Object System.IO.StreamWriter($pipe)
            $w.WriteLine("quit"); $w.Flush(); $w.Dispose()
        } catch {
            $p.CloseMainWindow() | Out-Null
        }
        $mine += $p.Id
    }
}
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline -and ($mine | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })) {
    Start-Sleep -Milliseconds 250
}
foreach ($id in $mine) {
    if (Get-Process -Id $id -ErrorAction SilentlyContinue) {
        Write-Output "copy $id did not close by itself - forcing it"
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
}
Set-Content -Path $file -Value "" -Encoding ascii
