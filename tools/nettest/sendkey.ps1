# Sends a key press (down + up) to a game window without focusing it.
# usage: powershell -File sendkey.ps1 -Key 2 [-ProcId 1234] [-HoldMs 80]
# Key is a single character (0-9, A-Z) or a virtual key code number.
param(
	[string]$Key,
	[int]$ProcId = 0,
	[int]$HoldMs = 80,
	[string]$Name = "pd.x86_64"
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KeySend {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
	[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@

if ($Key.Length -eq 1) { $vk = [int][char]$Key.ToUpper() } else { $vk = [int]$Key }
$sc = [KeySend]::MapVirtualKey($vk, 0)

$procs = Get-Process -Name $Name -ErrorAction SilentlyContinue
if ($ProcId) { $procs = $procs | Where-Object { $_.Id -eq $ProcId } }
foreach ($p in $procs) {
	$h = $p.MainWindowHandle
	if ($h -eq [IntPtr]::Zero) { continue }
	# arrows, Insert/Delete, Home/End and PgUp/PgDn are extended keys (bit 24)
	$ext = 0
	if ($vk -ge 0x21 -and $vk -le 0x2E) { $ext = 1 -shl 24 }
	$down = [IntPtr](1 -bor ($sc -shl 16) -bor $ext)
	$up = [IntPtr]([int64]1 -bor ([int64]$sc -shl 16) -bor $ext -bor (1 -shl 30) -bor ([int64]1 -shl 31))
	[KeySend]::PostMessage($h, 0x0100, [IntPtr]$vk, $down) | Out-Null
	Start-Sleep -Milliseconds $HoldMs
	[KeySend]::PostMessage($h, 0x0101, [IntPtr]$vk, $up) | Out-Null
	Write-Output ("sent {0} to pid {1}" -f $Key, $p.Id)
}
