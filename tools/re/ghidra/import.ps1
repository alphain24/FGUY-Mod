# Research: build the analyzed Ghidra projects decompile.ps1 reads (C:\Users\<you>\Tools\ghidra_projects).
#   import.ps1 pc   <FGUY.exe>                - the PC game (project FGUY)
#   import.ps1 x360 <x360_default.exe.bin>    - the Xbox 360 build's decrypted memory image (project X360, tools/re/xex.ps1)
# Takes a while (the analysis): run it in the background.
param([Parameter(Mandatory = $true)][ValidateSet("pc", "x360")][string]$Which, [Parameter(Mandatory = $true)][string]$File)
$tools = Join-Path $env:USERPROFILE "Tools"
$env:JAVA_HOME = (Get-ChildItem $tools -Directory -Filter "jdk-21*" | Select-Object -First 1).FullName
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
$ghidra = Join-Path $tools "ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat"
$project = Join-Path $tools "ghidra_projects"
New-Item -ItemType Directory -Force $project | Out-Null
if ($Which -eq "pc") {
    & $ghidra $project FGUY -import $File -overwrite 2>&1 | Select-String -Pattern "ERROR|Exception|Import succeeded|ANALYZING"
} else {
    # .pdata: 0x820e7200, 0x30310 bytes (the image's PE header) - one function per entry
    & $ghidra $project X360 -import $File -overwrite -loader BinaryLoader -loader-baseAddr 0x82000000 `
        -processor "PowerPC:BE:64:A2ALT-32addr" -scriptPath $PSScriptRoot -preScript X360Functions.java 820e7200 30310 2>&1 |
        Select-String -Pattern "ERROR|Exception|Import succeeded|X360Functions|ANALYZING"
}
