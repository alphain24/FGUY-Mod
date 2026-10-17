# Research: sends one command line to a test copy's pipe (see src\loader\vpad.cpp for the commands).
#   pipe.ps1 -Id 1234 hold 1 2000 ly=32767 rt=255
#   pipe.ps1 -Id 1234 pokef 43944c60 100
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][int]$Id,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Words
)
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$Id", [System.IO.Pipes.PipeDirection]::Out)
$pipe.Connect(3000)
$w = New-Object System.IO.StreamWriter($pipe)
$w.WriteLine($Words -join ' ')
$w.Flush(); $w.Dispose()
