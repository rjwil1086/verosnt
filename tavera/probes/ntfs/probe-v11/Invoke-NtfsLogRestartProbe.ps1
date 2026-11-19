# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v11/Invoke-NtfsLogRestartProbe.ps1
# PURPOSE:     Analyze probe-v7 $LogFile dumps for restart-page mount recipe (#2)
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Host-only follow-on to probe-v7. Does not boot Windows again.
# Requires output/ntfs/probe-v7/<stage>/captures/logfile_data.bin.
# Cite tavera/probes/ntfs/probe-v11 + restart-report.txt marker.
# Does not edit drivers/filesystems/ntfs.

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$V7 = Join-Path $SourceRoot 'output\ntfs\probe-v7'
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v11'
if (-not (Test-Path (Join-Path $V7 'F\captures\logfile_data.bin'))) {
    throw "probe-v7 dumps missing under $V7 — run probe-v7 first"
}
& (Join-Path $Probe 'host\Analyze-LogRestart.ps1') -V7Root $V7 -OutRoot $Out
$report = Join-Path $Out 'restart-report.txt'
$raw = Get-Content $report -Raw
if ($raw -notmatch 'NTFS-LOG-RESTART-REPORT-END') { throw 'restart report incomplete' }
if ($raw -notmatch 'SLOT_U64_AT_60_AND_68') { throw 'missing empty-slot observation' }
Write-Host 'probe-v11 log-restart analysis complete — update tavera/specs/ntfs from restart-report.txt'
Write-Host 'Do not edit drivers/filesystems/ntfs in this probe session.'
