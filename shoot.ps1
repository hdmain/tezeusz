$exe = "C:\Users\makss\Desktop\scoped\projects\tezeusz\build-msys\seerr.exe"
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
Start-Sleep -Seconds 8
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$bmp.Save("C:\Users\makss\Desktop\scoped\projects\tezeusz\shot1.png")
$g.Dispose(); $bmp.Dispose()
# bring seerr window to front and shoot again if still alive
if (-not $p.HasExited) {
    Add-Type @"
using System;using System.Runtime.InteropServices;
public class W{[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);[DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int n);}
"@
    [W]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
    [W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Seconds 2
    $bmp2 = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $g2 = [System.Drawing.Graphics]::FromImage($bmp2)
    $g2.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bmp2.Save("C:\Users\makss\Desktop\scoped\projects\tezeusz\shot2.png")
    $g2.Dispose(); $bmp2.Dispose()
    Stop-Process -Id $p.Id -Force
    "STILL-RUNNING (window handle: $($p.MainWindowHandle))"
} else {
    "EXITED code $($p.ExitCode)"
}
