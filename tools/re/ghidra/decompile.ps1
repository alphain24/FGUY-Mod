# Research: decompile functions of FGUY.exe with Ghidra (headless, the analyzed project in C:\Users\<you>\Tools\ghidra_projects).
#   decompile.ps1 <out.txt> <hex address | callers:<hex address>> [...]
param([Parameter(Mandatory = $true)][string]$Out, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Addresses)
$ghidra = Join-Path $env:USERPROFILE "Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat"
$project = Join-Path $env:USERPROFILE "Tools\ghidra_projects"
& $ghidra $project FGUY -process FGUY.exe -noanalysis -readOnly -scriptPath $PSScriptRoot -postScript Decompile.java $Out @Addresses 2>&1 |
    Select-String -Pattern "ERROR|Exception" | Select-Object -First 10
if (Test-Path $Out) { "-> $Out ($((Get-Content $Out | Measure-Object -Line).Lines) lines)" }
