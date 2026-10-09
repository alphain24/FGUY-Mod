# Closes only the game copies that test scripts started (their process ids are recorded in
# <game>\FGUYMod\test_pids.txt), never a copy you started yourself. Waits until they have exited.
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse")
$file = Join-Path $GameDir "FGUYMod\test_pids.txt"
if (-not (Test-Path $file)) { return }
$mine = @()
foreach ($id in Get-Content $file) {
    if (-not $id) { continue }
    $p = Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "FGUY") {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $mine += $p.Id
    }
}
Set-Content -Path $file -Value "" -Encoding ascii
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline -and ($mine | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })) {
    Start-Sleep -Milliseconds 200
}
