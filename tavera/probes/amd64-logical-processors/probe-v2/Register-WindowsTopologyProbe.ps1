# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/amd64-logical-processors/probe-v2/Register-WindowsTopologyProbe.ps1
# PURPOSE:     Register the topology probe in the Windows logon RunOnce key
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

param(
    [Parameter(Mandatory = $true)][string]$Vhd,
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Shape,
    [Parameter(Mandatory = $true)][string]$Log
)

$ErrorActionPreference = 'Stop'
$Buildenv = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..\..\buildenv')).Path
. (Join-Path $Buildenv 'differential-tests\Set-ProbeRunOnce.ps1')

try {
    $mounted = Mount-DiskImage -ImagePath $Vhd -PassThru
    Start-Sleep -Seconds 3
    try {
        $disk = Get-DiskImage -ImagePath $Vhd | Get-Disk
        if ($disk.IsOffline) { Set-Disk -Number $disk.Number -IsOffline $false }
        if ($disk.IsReadOnly) { Set-Disk -Number $disk.Number -IsReadOnly $false }
        $letter = $null
        foreach ($part in (Get-Partition -DiskNumber $disk.Number)) {
            if (-not $part.DriveLetter) { continue }
            if (Test-Path "$($part.DriveLetter):\Windows\System32") {
                $letter = $part.DriveLetter
                break
            }
        }
        if (-not $letter) { throw 'The Windows partition did not mount' }
        $shapeFile = Join-Path $env:TEMP "topology-shape-$Shape.txt"
        [System.IO.File]::WriteAllText($shapeFile, $Shape)
        Set-ProbeRunOnce -Letter $letter -Command 'C:\temp\topology_probe.exe' -Files @{
            'topology_probe.exe' = $Exe
            'shape.txt' = $shapeFile
        }
    }
    finally {
        Dismount-DiskImage -ImagePath $Vhd | Out-Null
    }
    Set-Content -Path $Log -Value 'ok' -Encoding ascii
}
catch {
    Set-Content -Path $Log -Value $_.Exception.Message -Encoding ascii
    exit 1
}
