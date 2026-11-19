# PROJECT: Tavera OS
# LICENSE: GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE: tavera/test-suites/ntfs/Register-WindowsProbe.ps1
# PURPOSE: Register the NTFS probe in the Windows logon RunOnce key
# COPYRIGHT: Copyright 2026 Ryan Wilson <ryan@verosnt.com>
#              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
#              Ryan Wilson <ryan@taveraos.com>

$ErrorActionPreference = 'Stop'
$Suite = $PSScriptRoot

function Find-ResolveVerosPaths {
    param([string]$Hint)
    $cursor = $Hint
    for ($i = 0; $i -lt 8; $i++) {
        $candidate = Join-Path $cursor 'buildenv\Resolve-VerosPaths.ps1'
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
        $atRoot = Join-Path $cursor 'Resolve-VerosPaths.ps1'
        if ((Test-Path -LiteralPath $atRoot) -and (Test-Path -LiteralPath (Join-Path $cursor 'env.cmd'))) {
            return (Resolve-Path -LiteralPath $atRoot).Path
        }
        $parent = Split-Path -Parent $cursor
        if (-not $parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
    throw "Resolve-VerosPaths.ps1 was not found from '$Hint'."
}

. (Find-ResolveVerosPaths -Hint $Suite)
$Root = Resolve-VerosBuildenvRoot -Hint $Suite
$Out = Join-Path $Suite 'out'
$Log = Join-Path $Out 'register-probe.log'
. (Join-Path $Root 'differential-tests\Set-ProbeRunOnce.ps1')
try {
    $sys = Join-Path $Out 'windows-system.vhdx'
    $agent = Join-Path $Out 'ntfs_agent.exe'
    $mounted = Mount-DiskImage -ImagePath $sys -PassThru
    Start-Sleep -Seconds 3
    try {
        $disk = Get-DiskImage -ImagePath $sys | Get-Disk
        if ($disk.IsOffline) { Set-Disk -Number $disk.Number -IsOffline $false }
        if ($disk.IsReadOnly) { Set-Disk -Number $disk.Number -IsReadOnly $false }
        $letter = $null
        foreach ($part in (Get-Partition -DiskNumber $disk.Number)) {
            if (-not $part.DriveLetter) { continue }
            if (Test-Path "$($part.DriveLetter):\Windows\System32") { $letter = $part.DriveLetter; break }
        }
        if (-not $letter) { throw 'The Windows partition did not mount' }
        Set-ProbeRunOnce -Letter $letter -Command 'C:\temp\ntfs_agent.exe' -Files @{ 'ntfs_agent.exe' = $agent }
    }
    finally {
        Dismount-DiskImage -ImagePath $sys | Out-Null
    }
    Set-Content -Path $Log -Value 'ok' -Encoding ascii
}
catch {
    Set-Content -Path $Log -Value $_.Exception.Message -Encoding ascii
    exit 1
}
