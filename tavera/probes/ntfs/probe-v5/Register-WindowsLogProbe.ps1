# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v5/Register-WindowsLogProbe.ps1
# PURPOSE:     Register the NTFS log probe RunOnce and stage letter
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

param(
    [Parameter(Mandatory = $true)][ValidateSet('C', 'W', 'D', 'X')][string]$Stage
)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v5'
$Log = Join-Path $Out 'register-probe.log'
$Buildenv = (Resolve-Path (Join-Path $SourceRoot '..\buildenv')).Path
. (Join-Path $Buildenv 'differential-tests\Set-ProbeRunOnce.ps1')
try {
    $sys = Join-Path $Out 'windows-system.vhdx'
    $agent = Join-Path $Out 'ntfs_log_probe.exe'
    $stageFile = Join-Path $Out "stage-$Stage.txt"
    Set-Content -Path $stageFile -Value $Stage -Encoding ascii -NoNewline
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
        Set-ProbeRunOnce -Letter $letter -Command 'C:\temp\ntfs_log_probe.exe' -Files @{
            'ntfs_log_probe.exe' = $agent
            'ntfs_log_stage.txt' = $stageFile
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
