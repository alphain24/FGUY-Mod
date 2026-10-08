# Saves a screenshot of the primary screen. Usage: screenshot.ps1 <output.png>
param([string]$Out)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Name Dpi -Namespace U -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[U.Dpi]::SetProcessDPIAware() | Out-Null  # capture real pixels on scaled (e.g. 150%) displays
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($b.Width / 2)), ([int]($b.Height / 2))
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $small.Dispose()
