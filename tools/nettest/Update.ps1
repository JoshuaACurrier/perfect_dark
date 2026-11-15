# Perfect Dark Online updater. Play.bat runs this before it starts the game.
#
# It asks the download address for manifest.txt, and if that names a newer build than
# the one in this folder (version.txt), downloads the program files that differ, checks
# each against the manifest's SHA-256, and swaps them in. Your ROM, save, settings, logs
# and texture packs are never touched: only the files the manifest lists are.
#
# No internet, no newer build, or any error: it says so in one line and the game starts
# as it is. To switch updates off, put an empty file called noupdate.txt in this folder.
#
# PD_UPDATE_URL overrides the address (for testing, or a copy kept somewhere else).

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$base = 'https://github.com/JoshuaACurrier/perfect_dark/releases/latest/download'
if ($env:PD_UPDATE_URL) { $base = $env:PD_UPDATE_URL.TrimEnd('/') }

function Get-LocalBuild {
	$path = Join-Path $here 'version.txt'
	if (Test-Path $path) {
		foreach ($line in Get-Content $path) {
			if ($line -match '^build\s+(\d+)') { return [int]$Matches[1] }
		}
	}
	return 0
}

try {
	[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

	$localBuild = Get-LocalBuild
	$manifestText = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 8 -Uri "$base/manifest.txt").Content
	if ($manifestText -is [byte[]]) { $manifestText = [Text.Encoding]::UTF8.GetString($manifestText) }

	$remoteBuild = 0
	$commit = ''
	$notes = @()
	$files = @()
	foreach ($line in ($manifestText -split "`r?`n")) {
		if ($line -match '^build\s+(\d+)') { $remoteBuild = [int]$Matches[1] }
		elseif ($line -match '^commit\s+(\S+)') { $commit = $Matches[1] }
		elseif ($line -match '^note\s+(.*)$') { $notes += $Matches[1] }
		elseif ($line -match '^file\s+(\S+)\s+([0-9a-fA-F]{64})\s+(\d+)') {
			$name = $Matches[1]
			$hash = $Matches[2].ToLower()
			$size = [int64]$Matches[3]
			# plain file names only: nothing with a path in it
			if ($name -match '^[A-Za-z0-9._-]+$' -and $name -notmatch '^\.+$') {
				$files += [pscustomobject]@{ Name = $name; Hash = $hash; Size = $size }
			}
		}
	}

	if ($remoteBuild -le 0 -or $files.Count -eq 0) { throw 'the update list could not be read' }

	if ($remoteBuild -le $localBuild) {
		Write-Host "Perfect Dark Online is up to date (build $localBuild)."
		exit 0
	}

	Write-Host "Updating Perfect Dark Online: build $localBuild -> $remoteBuild ($commit)"
	foreach ($n in $notes) { Write-Host "  $n" }

	# download everything that differs first, so a failure half way changes nothing
	$pending = @()
	foreach ($f in $files) {
		$target = Join-Path $here $f.Name
		if (Test-Path $target) {
			$have = (Get-FileHash -Algorithm SHA256 -Path $target).Hash.ToLower()
			if ($have -eq $f.Hash) { continue }
		}
		$tmp = "$target.new"
		Write-Host ("  downloading {0} ({1:N1} MB)" -f $f.Name, ($f.Size / 1MB))
		Invoke-WebRequest -UseBasicParsing -TimeoutSec 300 -Uri "$base/$($f.Name)" -OutFile $tmp
		$got = (Get-FileHash -Algorithm SHA256 -Path $tmp).Hash.ToLower()
		if ($got -ne $f.Hash) {
			Remove-Item -Force $tmp
			throw "$($f.Name) did not download correctly"
		}
		$pending += [pscustomobject]@{ Tmp = $tmp; Target = $target }
	}

	foreach ($p in $pending) {
		Move-Item -Force -Path $p.Tmp -Destination $p.Target
	}

	Set-Content -Encoding ASCII -Path (Join-Path $here 'version.txt') -Value @("build $remoteBuild", "commit $commit")
	Write-Host "Updated to build $remoteBuild."
	exit 0
} catch {
	Get-ChildItem -Path $here -Filter '*.new' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
	Write-Host "Update check skipped: $($_.Exception.Message)"
	Write-Host "(If the game is already running, close it and start Play.bat again to update.)"
	exit 0
}
