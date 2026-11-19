# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v12/Invoke-NtfsMountDeepProbe.ps1
# PURPOSE:     Deep host analysis of chkdsk-named FRS after AllocatedSize-only retest
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Host-only follow-on to probe-v8 + issue-22 AllocatedSize retest.
# Does not boot Windows. Does not edit drivers/filesystems/ntfs.
# Cite tavera/probes/ntfs/probe-v12 + mount-deep-report.txt marker.

param(
    [string]$Image = ''
)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v12'
$V8Raw = Join-Path $SourceRoot 'output\ntfs\probe-v8\kept-disk.raw'

if (-not $Image) {
    $Image = $V8Raw
}
if (-not (Test-Path $Image)) {
    throw "kept disk raw missing: $Image — run probe-v8 convert first"
}

& (Join-Path $Probe 'host\Analyze-MountDeep.ps1') -Image $Image -OutRoot $Out
$report = Join-Path $Out 'mount-deep-report.txt'
$raw = Get-Content $report -Raw
if ($raw -notmatch 'NTFS-MOUNT-DEEP-REPORT-END') { throw 'mount-deep report incomplete' }
if ($raw -notmatch 'FAIL J_sparse_flag_without_holes') {
    throw 'expected FAIL J_sparse_flag_without_holes on kept disk'
}
if ($raw -notmatch 'FAIL orphan_frs') {
    throw 'expected orphan FRS FAIL on kept disk'
}
Write-Host 'probe-v12 mount-deep complete — update tavera/specs/ntfs from mount-deep-report.txt'
Write-Host 'Do not edit drivers/filesystems/ntfs in this probe session.'
