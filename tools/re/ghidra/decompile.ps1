# Research: decompile functions with Ghidra (headless, the analyzed projects in C:\Users\<you>\Tools\ghidra_projects -
# made by import.ps1). FGUY.exe by default; -X360 the Xbox 360 build.
#   decompile.ps1 [-X360] <out.txt> <hex address | callers:<hex address>> [...]
param([switch]$X360, [Parameter(Mandatory = $true)][string]$Out, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Addresses)
$tools = Join-Path $env:USERPROFILE "Tools"
$env:JAVA_HOME = (Get-ChildItem $tools -Directory -Filter "jdk-21*" | Select-Object -First 1).FullName
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
$ghidra = Join-Path $tools "ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat"
$project = Join-Path $tools "ghidra_projects"
$name, $file = if ($X360) { "X360", "x360_default.exe.bin" } else { "FGUY", "FGUY.exe" }
& $ghidra $project $name -process $file -noanalysis -readOnly -scriptPath $PSScriptRoot -postScript Decompile.java $Out @Addresses 2>&1 |
    Select-String -Pattern "ERROR|Exception" | Select-Object -First 10
if (Test-Path $Out) { "-> $Out ($((Get-Content $Out | Measure-Object -Line).Lines) lines)" }
