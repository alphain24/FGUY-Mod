# Screenshot of the game window's own picture, even when it's behind other windows.
# Usage: window_shot.ps1 <out.png> [-Scale 0.5] [-Id <process id>]
param([string]$Out, [double]$Scale = 0.5, [int]$Id = 0)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
[W]::SetProcessDPIAware() | Out-Null
$h = $(if ($Id) { Get-Process -Id $Id } else { Get-Process FGUY | Select-Object -First 1 }).MainWindowHandle
$r = New-Object W+RECT
[W]::GetClientRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[W]::PrintWindow($h, $hdc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT (works for Direct3D)
$g.ReleaseHdc($hdc)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($bmp.Width * $Scale)), ([int]($bmp.Height * $Scale))
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $small.Dispose()
