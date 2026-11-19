# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v7/host/Diff-LogIso.ps1
# PURPOSE:     Diff single-op $LogFile captures for LOG-001 recipe
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

param(
    [string]$OutRoot = ''
)

$ErrorActionPreference = 'Stop'
$Probe = Split-Path $PSScriptRoot -Parent
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
if (-not $OutRoot) { $OutRoot = Join-Path $SourceRoot 'output\ntfs\probe-v7' }
$Report = Join-Path $OutRoot 'log-iso-report.txt'

function Get-Runs {
    param([byte[]]$Left, [byte[]]$Right)
    $n = [Math]::Min($Left.Length, $Right.Length)
    $runs = New-Object System.Collections.Generic.List[object]
    $i = 0
    while ($i -lt $n) {
        if ($Left[$i] -eq $Right[$i]) { $i++; continue }
        $start = $i
        while ($i -lt $n -and $Left[$i] -ne $Right[$i]) { $i++ }
        $len = $i - $start
        $aHex = ($Left[$start..([Math]::Min($start + 15, $start + $len - 1))] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $bHex = ($Right[$start..([Math]::Min($start + 15, $start + $len - 1))] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $runs.Add([pscustomobject]@{ Offset = $start; Length = $len; A = $aHex; B = $bHex })
    }
    if ($Left.Length -ne $Right.Length) {
        $runs.Add([pscustomobject]@{
            Offset = $n; Length = [Math]::Abs($Right.Length - $Left.Length)
            A = "len=$($Left.Length)"; B = "len=$($Right.Length)"
        })
    }
    return $runs
}

function Get-FixupCadenceHint {
    param($Runs)
    $hits = 0
    $checked = 0
    foreach ($r in $Runs) {
        if ($checked -ge 64) { break }
        $checked++
        if ($r.Length -eq 1 -and (($r.Offset + 2) % 512) -eq 0) { $hits++ }
    }
    return $hits
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('NTFS-LOG-ISO-REPORT')
foreach ($s in @('F', 'C', 'W', 'N', 'X', 'T')) {
    $p = Join-Path $OutRoot "$s\captures\logfile_data.bin"
    $ser = Join-Path $OutRoot "$s\guest-serial.log"
    if (Test-Path $p) {
        $bytes = [IO.File]::ReadAllBytes($p)
        $ff = 0; foreach ($b in $bytes) { if ($b -eq 0xFF) { $ff++ } }
        $lines.Add("STAGE`t$s`tSIZE`t$($bytes.Length)`tFF_BYTES`t$ff`tPATH`t$p")
    } else {
        $lines.Add("STAGE`t$s`tMISSING_LOG")
    }
    if (Test-Path $ser) {
        $t = Get-Content $ser -Raw
        if ($t -match 'create empty ok') { $lines.Add("STAGE`t$s`tCREATE\tok") }
        if ($t -match 'write known ok') { $lines.Add("STAGE`t$s`tWRITE\tok") }
        if ($t -match 'rename ok') { $lines.Add("STAGE`t$s`tRENAME\tok") }
        if ($t -match 'delete ok') { $lines.Add("STAGE`t$s`tDELETE\tok") }
        if ($t -match 'readback ok') { $lines.Add("STAGE`t$s`tREADBACK\tok") }
        if ($t -match 'remount NTFS ok') { $lines.Add("STAGE`t$s`tREMOUNT\tok") }
        if ($t -match 'fsutil dirty set ok') { $lines.Add("STAGE`t$s`tDIRTY\tok") }
    }
}

$pairs = @(
    @{ L = 'F'; R = 'C'; Name = 'format-to-create' },
    @{ L = 'C'; R = 'W'; Name = 'create-to-write' },
    @{ L = 'W'; R = 'N'; Name = 'write-to-rename' },
    @{ L = 'N'; R = 'X'; Name = 'rename-to-delete' },
    @{ L = 'W'; R = 'T'; Name = 'write-to-dirty-remount' }
)
foreach ($p in $pairs) {
    $lp = Join-Path $OutRoot "$($p.L)\captures\logfile_data.bin"
    $rp = Join-Path $OutRoot "$($p.R)\captures\logfile_data.bin"
    $lines.Add("PAIR`t$($p.Name)")
    if (-not (Test-Path $lp) -or -not (Test-Path $rp)) {
        $lines.Add('MISSING')
        continue
    }
    $left = [IO.File]::ReadAllBytes($lp)
    $right = [IO.File]::ReadAllBytes($rp)
    $runs = Get-Runs $left $right
    $fixupHits = Get-FixupCadenceHint $runs
    $lines.Add("SIZES`t$($left.Length)`t$($right.Length)`tDIFFRUNS`t$($runs.Count)`tFIXUP_CADENCE_HITS`t$fixupHits")
    $show = [Math]::Min(24, $runs.Count)
    for ($i = 0; $i -lt $show; $i++) {
        $r = $runs[$i]
        $lines.Add("RUN`t$($r.Offset)`t$($r.Length)`t$($r.A)`t$($r.B)")
    }
}
$lines.Add('NTFS-LOG-ISO-REPORT-END')
[IO.File]::WriteAllLines($Report, $lines)
Write-Host "Wrote $Report"
Get-Content $Report | Select-Object -First 100
