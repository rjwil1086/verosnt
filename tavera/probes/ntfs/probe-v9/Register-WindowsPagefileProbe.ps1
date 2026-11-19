# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v9/Register-WindowsPagefileProbe.ps1
# PURPOSE:     Register the NTFS pagefile probe RunOnce
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v9'
$Log = Join-Path $Out 'register-probe.log'
$Buildenv = (Resolve-Path (Join-Path $SourceRoot '..\buildenv')).Path
. (Join-Path $Buildenv 'differential-tests\Set-ProbeRunOnce.ps1')
try {
    $sys = Join-Path $SourceRoot 'output\ntfs\windows-system.vhdx'
    $agent = Join-Path $Out 'ntfs_pagefile_probe.exe'
    if (-not (Test-Path $sys)) { throw "missing $sys" }
    if (-not (Test-Path $agent)) { throw "missing $agent" }
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
        Set-ProbeRunOnce -Letter $letter -Command 'C:\temp\ntfs_pagefile_probe.exe' -Files @{
            'ntfs_pagefile_probe.exe' = $agent
        }
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
