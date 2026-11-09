# Screenshots every window of a process (default pd.x86_64) into PNG files,
# without capturing the rest of the desktop.
# usage: powershell -File winshot.ps1 [-OutDir dir] [-Name pd.x86_64]
param(
	[string]$OutDir = ".",
	[string]$Name = "pd.x86_64"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinShot {
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
	[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[WinShot]::SetProcessDPIAware() | Out-Null

New-Item -ItemType Directory -Force $OutDir | Out-Null
$i = 0
foreach ($p in Get-Process -Name $Name -ErrorAction SilentlyContinue) {
	$h = $p.MainWindowHandle
	if ($h -eq [IntPtr]::Zero) { continue }
	$r = New-Object WinShot+RECT
	[WinShot]::GetClientRect($h, [ref]$r) | Out-Null
	$w = $r.R - $r.L; $hh = $r.B - $r.T
	if ($w -le 0 -or $hh -le 0) { continue }
	$bmp = New-Object System.Drawing.Bitmap $w, $hh
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$hdc = $g.GetHdc()
	# 3 = PW_CLIENTONLY | PW_RENDERFULLCONTENT (needed for GL windows)
	[WinShot]::PrintWindow($h, $hdc, 3) | Out-Null
	$g.ReleaseHdc($hdc)
	$file = Join-Path $OutDir ("shot_{0}_{1}.png" -f $i, $p.Id)
	$bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
	$g.Dispose(); $bmp.Dispose()
	Write-Output $file
	$i++
}
