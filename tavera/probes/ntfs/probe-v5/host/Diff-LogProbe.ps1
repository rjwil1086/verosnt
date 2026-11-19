# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v5/host/Diff-LogProbe.ps1
# PURPOSE:     Diff staged $LogFile captures for recovery citations
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

param(
    [string]$OutRoot = ''
)

$ErrorActionPreference = 'Stop'
$Probe = Split-Path $PSScriptRoot -Parent
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
if (-not $OutRoot) { $OutRoot = Join-Path $SourceRoot 'output\ntfs\probe-v5' }
$Report = Join-Path $OutRoot 'log-report.txt'

function Get-Runs {
    param([byte[]]$Left, [byte[]]$Right, [int]$MaxShow = 32)
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

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('NTFS-LOG-PROBE-REPORT')
foreach ($s in @('C', 'W', 'D', 'X')) {
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
        if ($t -match 'readback ok') { $lines.Add("STAGE`t$s`tREADBACK\tok") }
        if ($t -match 'remount NTFS ok') { $lines.Add("STAGE`t$s`tREMOUNT\tok") }
        if ($t -match 'fsutil dirty set ok') { $lines.Add("STAGE`t$s`tDIRTY\tok") }
    }
}

$pairs = @(
    @{ L = 'C'; R = 'W'; Name = 'format-to-write' },
    @{ L = 'W'; R = 'D'; Name = 'write-to-dirty-remount' },
    @{ L = 'W'; R = 'X'; Name = 'write-to-extra-traffic' }
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
    $lines.Add("SIZES`t$($left.Length)`t$($right.Length)`tDIFFRUNS`t$($runs.Count)")
    $show = [Math]::Min(24, $runs.Count)
    for ($i = 0; $i -lt $show; $i++) {
        $r = $runs[$i]
        $lines.Add("RUN`t$($r.Offset)`t$($r.Length)`t$($r.A)`t$($r.B)")
    }
}
$lines.Add('NTFS-LOG-PROBE-REPORT-END')
[IO.File]::WriteAllLines($Report, $lines)
Write-Host "Wrote $Report"
Get-Content $Report | Select-Object -First 80
