# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v3/host/Diff-QuotaGrowth.ps1
# PURPOSE:     Diff staged $Q index-root captures for growth citations
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

param(
    [string]$OutRoot = ''
)

$ErrorActionPreference = 'Stop'
$Probe = Split-Path $PSScriptRoot -Parent
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
if (-not $OutRoot) { $OutRoot = Join-Path $SourceRoot 'output\ntfs\probe-v3' }
$Report = Join-Path $OutRoot 'growth-report.txt'

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
        $aHex = ($Left[$start..([Math]::Min($start + 31, $start + $len - 1))] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $bHex = ($Right[$start..([Math]::Min($start + 31, $start + $len - 1))] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $runs.Add([pscustomobject]@{ Offset = $start; Length = $len; A = $aHex; B = $bHex })
    }
    if ($Left.Length -ne $Right.Length) {
        $extraStart = $n
        $longer = if ($Right.Length -gt $Left.Length) { $Right } else { $Left }
        $extraLen = [Math]::Abs($Right.Length - $Left.Length)
        $tail = ($longer[$extraStart..([Math]::Min($extraStart + 63, $longer.Length - 1))] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $runs.Add([pscustomobject]@{
            Offset = $extraStart; Length = $extraLen
            A = "len=$($Left.Length)"; B = "len=$($Right.Length) tail=$tail"
        })
    }
    return $runs
}

function Read-I64LE([byte[]]$b, [int]$off) {
    if ($off + 8 -gt $b.Length) { return $null }
    return [BitConverter]::ToInt64($b, $off)
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('NTFS-QUOTA-GROWTH-REPORT')
$stages = @('F', 'T', 'M', 'D')
foreach ($s in $stages) {
    $p = Join-Path $OutRoot "$s\captures\quota_q_index_root.bin"
    if (Test-Path $p) {
        $bytes = [IO.File]::ReadAllBytes($p)
        $t316 = Read-I64LE $bytes 316
        $t324 = Read-I64LE $bytes 324
        $lines.Add("STAGE`t$s`tSIZE`t$($bytes.Length)`tI64@316`t$t316`tI64@324`t$t324`t$path")
    } else {
        $lines.Add("STAGE`t$s`tMISSING")
    }
}

$pairs = @(
    @{ L = 'F'; R = 'T'; Name = 'format-to-track' },
    @{ L = 'T'; R = 'M'; Name = 'track-to-modify' },
    @{ L = 'M'; R = 'D'; Name = 'modify-to-delete' },
    @{ L = 'F'; R = 'M'; Name = 'format-to-modify' }
)
foreach ($p in $pairs) {
    $lp = Join-Path $OutRoot "$($p.L)\captures\quota_q_index_root.bin"
    $rp = Join-Path $OutRoot "$($p.R)\captures\quota_q_index_root.bin"
    $lines.Add("PAIR`t$($p.Name)")
    if (-not (Test-Path $lp) -or -not (Test-Path $rp)) {
        $lines.Add('MISSING')
        continue
    }
    $left = [IO.File]::ReadAllBytes($lp)
    $right = [IO.File]::ReadAllBytes($rp)
    $runs = Get-Runs $left $right
    $lines.Add("SIZES`t$($left.Length)`t$($right.Length)`tDIFFRUNS`t$($runs.Count)")
    $show = [Math]::Min(48, $runs.Count)
    for ($i = 0; $i -lt $show; $i++) {
        $r = $runs[$i]
        $lines.Add("RUN`t$($r.Offset)`t$($r.Length)`t$($r.A)`t$($r.B)")
    }
}

$lines.Add('NTFS-QUOTA-GROWTH-REPORT-END')
[IO.File]::WriteAllLines($Report, $lines)
Write-Host "Wrote $Report"
Get-Content $Report
