# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v4/host/Diff-QuotaDelete.ps1
# PURPOSE:     Diff modify vs NtSetQuota -2 $Q captures
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

param(
    [string]$OutRoot = ''
)

$ErrorActionPreference = 'Stop'
$Probe = Split-Path $PSScriptRoot -Parent
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
if (-not $OutRoot) { $OutRoot = Join-Path $SourceRoot 'output\ntfs\probe-v4' }
$Report = Join-Path $OutRoot 'delete-report.txt'

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
        $runs.Add([pscustomobject]@{
            Offset = $n; Length = [Math]::Abs($Right.Length - $Left.Length)
            A = "len=$($Left.Length)"; B = "len=$($Right.Length)"
        })
    }
    return $runs
}

function Read-I64LE([byte[]]$b, [int]$off) {
    if ($off + 8 -gt $b.Length) { return $null }
    return [BitConverter]::ToInt64($b, $off)
}

function Walk-Keys([byte[]]$b) {
    $keys = New-Object System.Collections.Generic.List[string]
    $p = 0x20
    while ($p + 0x10 -le $b.Length) {
        $entryLen = [BitConverter]::ToUInt16($b, $p + 8)
        $keyLen = [BitConverter]::ToUInt16($b, $p + 0xA)
        $flags = [BitConverter]::ToUInt16($b, $p + 0xC)
        if (($flags -band 2) -ne 0 -or $entryLen -eq 0) { break }
        if ($keyLen -ge 4) {
            $k = [BitConverter]::ToUInt32($b, $p + 0x10)
            $keys.Add(("0x{0:X}" -f $k))
        }
        if ($entryLen -lt 0x10) { break }
        $p += $entryLen
    }
    return ($keys -join ',')
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('NTFS-QUOTA-DELETE-REPORT')
foreach ($s in @('M', 'Q')) {
    $p = Join-Path $OutRoot "$s\captures\quota_q_index_root.bin"
    if (Test-Path $p) {
        $bytes = [IO.File]::ReadAllBytes($p)
        $lines.Add("STAGE`t$s`tSIZE`t$($bytes.Length)`tI64@316`t$(Read-I64LE $bytes 316)`tI64@324`t$(Read-I64LE $bytes 324)`tKEYS`t$(Walk-Keys $bytes)")
    } else {
        $lines.Add("STAGE`t$s`tMISSING")
    }
}

$lp = Join-Path $OutRoot 'M\captures\quota_q_index_root.bin'
$rp = Join-Path $OutRoot 'Q\captures\quota_q_index_root.bin'
$lines.Add('PAIR`tmodify-to-NtSetMinus2')
if ((Test-Path $lp) -and (Test-Path $rp)) {
    $left = [IO.File]::ReadAllBytes($lp)
    $right = [IO.File]::ReadAllBytes($rp)
    $runs = Get-Runs $left $right
    $lines.Add("SIZES`t$($left.Length)`t$($right.Length)`tDIFFRUNS`t$($runs.Count)")
    $show = [Math]::Min(48, $runs.Count)
    for ($i = 0; $i -lt $show; $i++) {
        $r = $runs[$i]
        $lines.Add("RUN`t$($r.Offset)`t$($r.Length)`t$($r.A)`t$($r.B)")
    }
} else {
    $lines.Add('MISSING')
}
$lines.Add('NTFS-QUOTA-DELETE-REPORT-END')
[IO.File]::WriteAllLines($Report, $lines)
Write-Host "Wrote $Report"
Get-Content $Report
