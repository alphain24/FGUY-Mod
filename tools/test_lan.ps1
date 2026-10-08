# Starts two windowed copies of the game on this PC: a network server and a network client.
# Each copy reads its own config from FGUYMod\ (server.ini / client.ini) and writes its own log.
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [string]$Res = "960x600",
    [int]$Delay = 30
)

Get-Process FGUY -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

function Start-Copy([string]$ini) {
    $env:FGUYMOD_INI = $ini
    $env:FGUYMOD_WINDOWED = "1"
    $env:FGUYMOD_RES = $Res
    $p = Start-Process -FilePath "$GameDir\FGUY.exe" -WorkingDirectory $GameDir -PassThru
    $env:FGUYMOD_INI = $null; $env:FGUYMOD_WINDOWED = $null; $env:FGUYMOD_RES = $null
    return $p
}

$server = Start-Copy "server.ini"
Write-Output "server pid $($server.Id)"
Start-Sleep -Seconds $Delay
$client = Start-Copy "client.ini"
Write-Output "client pid $($client.Id)"
