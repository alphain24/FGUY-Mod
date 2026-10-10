# Test tool: brings a game copy to the front and sends it real keyboard / mouse input (SendInput).
#   kbm_send.ps1 -Id 1234 -Steps "key:W:2000", "mouse:25:0:60", "key:Space:100"
#     key:<name>:<ms>          hold a key (W, A, S, D, Space, Enter, Escape, Shift, R, E, ...) for ms
#     mouse:<dx>:<dy>:<count>  move the mouse by dx, dy every 16 ms, count times
#     click:<left|right>:<ms>  hold a mouse button
#     wait:<ms>
# Only for test copies on this PC: it takes the keyboard focus while it runs.
param([int]$Id, [string[]]$Steps)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class FgKbm {
  [StructLayout(LayoutKind.Sequential)] public struct MI { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk, scan; public uint flags, time; public IntPtr extra; public uint pad1, pad2; }
  // INPUT: the union starts at 8 in 64-bit processes (it holds a pointer), 4 in 32-bit ones
  [StructLayout(LayoutKind.Explicit)] public struct IN64 { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MI mi; [FieldOffset(8)] public KI ki; }
  [StructLayout(LayoutKind.Explicit)] public struct IN32 { [FieldOffset(0)] public uint type; [FieldOffset(4)] public MI mi; [FieldOffset(4)] public KI ki; }
  [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, IN64[] i, int size);
  [DllImport("user32.dll", SetLastError = true, EntryPoint = "SendInput")] static extern uint SendInput32(uint n, IN32[] i, int size);
  static void Send(uint type, MI mi, KI ki) {
    uint sent;
    if (IntPtr.Size == 8) { var i = new IN64[1]; i[0].type = type; if (type == 0) i[0].mi = mi; else i[0].ki = ki; sent = SendInput(1, i, Marshal.SizeOf(typeof(IN64))); }
    else { var i = new IN32[1]; i[0].type = type; if (type == 0) i[0].mi = mi; else i[0].ki = ki; sent = SendInput32(1, i, Marshal.SizeOf(typeof(IN32))); }
    if (sent != 1) throw new Exception("SendInput failed: " + Marshal.GetLastWin32Error());
  }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public static void Key(ushort vk, bool up) { var k = new KI(); k.vk = vk; k.flags = up ? 2u : 0u; Send(1, new MI(), k); }
  public static void Mouse(int dx, int dy, uint flags) { var m = new MI(); m.dx = dx; m.dy = dy; m.flags = flags; Send(0, m, new KI()); }
}
"@
$vk = @{ Space = 0x20; Enter = 0x0D; Escape = 0x1B; Shift = 0x10; Tab = 0x09; Back = 0x08; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27 }
$game = Get-Process -Id $Id
[FgKbm]::Mouse(0, 0, 0x10)   # release the right button (an earlier broken tool run could leave it down)
for ($try = 0; $try -lt 5 -and [FgKbm]::GetForegroundWindow() -ne $game.MainWindowHandle; $try++) {
    [FgKbm]::Key(0x12, $false); [FgKbm]::Key(0x12, $true)   # a tap of Alt lets us change the front window
    [FgKbm]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 300
}
if ([FgKbm]::GetForegroundWindow() -ne $game.MainWindowHandle) { Write-Output "could not bring $Id to the front"; exit 1 }
Start-Sleep -Milliseconds 200
foreach ($s in $Steps) {
    $f = $s.Split(':')
    switch ($f[0]) {
        'key' {
            $code = if ($vk.ContainsKey($f[1])) { $vk[$f[1]] } else { [int][char]$f[1].ToUpper() }
            [FgKbm]::Key([uint16]$code, $false); Start-Sleep -Milliseconds ([int]$f[2]); [FgKbm]::Key([uint16]$code, $true)
        }
        'mouse' { for ($i = 0; $i -lt [int]$f[3]; $i++) { [FgKbm]::Mouse([int]$f[1], [int]$f[2], 1); Start-Sleep -Milliseconds 16 } }
        'click' {
            $down, $up = if ($f[1] -eq 'right') { 0x8, 0x10 } else { 0x2, 0x4 }
            [FgKbm]::Mouse(0, 0, $down); Start-Sleep -Milliseconds ([int]$f[2]); [FgKbm]::Mouse(0, 0, $up)
        }
        'wait' { Start-Sleep -Milliseconds ([int]$f[1]) }
    }
}
Write-Output "sent: $($Steps -join ', ')"
