@echo off
rem Closes every test bot ("Start test bot.bat") the normal way. Never touches the copy you play.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$f = Join-Path '%~dp0' 'bot_pids.txt'; if (-not (Test-Path $f)) { 'No test bot running.'; exit };" ^
  "foreach ($id in Get-Content $f) { if (-not $id) { continue }; $p = Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue;" ^
  "  if ($p -and $p.ProcessName -eq 'FGUY') { try { $c = New-Object System.IO.Pipes.NamedPipeClientStream('.', ('FGUYMod_' + $p.Id), [System.IO.Pipes.PipeDirection]::Out);" ^
  "    $c.Connect(2000); $w = New-Object System.IO.StreamWriter($c); $w.WriteLine('quit'); $w.Flush(); $w.Dispose(); 'Closed test bot ' + $p.Id } catch { $p.CloseMainWindow() | Out-Null } } };" ^
  "Remove-Item $f"
timeout /t 3 >nul 2>&1
