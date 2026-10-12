# Decompiles the game functions containing the given addresses (Ghidra, headless) into build\decomp.txt.
#   decompile.ps1 5a0280 61ea40 ...
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Addresses)
$P = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $P "build\decomp.txt"
$env:JAVA_HOME = "C:\Program Files\Java\jdk-25.0.2"
& "C:\Users\alpha\Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "C:\Users\alpha\Tools\ghidra_projects" FGUY `
    -process FGUY.exe -noanalysis -readOnly -scriptPath $PSScriptRoot -postScript Decompile.java $out @Addresses 2>&1 |
    Select-String -Pattern "ERROR|Exception" | Select-Object -First 5
Get-Content $out
