# Test tool: keeps the screen saver from starting while tests run - it takes the test copies' screen away (their
# Direct3D device is lost a minute after the last real input; the virtual pad isn't input to Windows). Holds a display
# power request and nudges the mouse 1 pixel there and back every 20 s, for -Hours; nothing is changed in Windows'
# settings, and it all ends with this process. Start it hidden in the background:
#   Start-Process powershell -WindowStyle Hidden -ArgumentList "-ExecutionPolicy Bypass -File tools\keep_screen_on.ps1"
param([double]$Hours = 3)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class FgPower {
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint data, UIntPtr extra);
}
"@
$until = (Get-Date).AddHours($Hours)
while ((Get-Date) -lt $until) {
    [FgPower]::SetThreadExecutionState(0x80000003) | Out-Null   # (continuous: system and display required)
    [FgPower]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero); [FgPower]::mouse_event(1, -1, 0, 0, [UIntPtr]::Zero)
    Start-Sleep 20
}
