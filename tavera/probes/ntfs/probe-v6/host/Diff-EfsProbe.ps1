# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v6/host/Diff-EfsProbe.ps1
# PURPOSE:     Diff staged $EFS / $DATA captures for EFS-002 recipe
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

param(
    [string]$OutRoot = ''
)

$ErrorActionPreference = 'Stop'
$Probe = Split-Path $PSScriptRoot -Parent
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
if (-not $OutRoot) { $OutRoot = Join-Path $SourceRoot 'output\ntfs\probe-v6' }
$Report = Join-Path $OutRoot 'efs-report.txt'

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

function Add-BlobLines([System.Collections.Generic.List[string]]$Lines, [string]$Stage, [string]$Name) {
    $p = Join-Path $OutRoot "$Stage\captures\$Name"
    if (Test-Path $p) {
        $bytes = [IO.File]::ReadAllBytes($p)
        $Lines.Add("STAGE`t$Stage`tBLOB`t$Name`tSIZE`t$($bytes.Length)`tPATH`t$p")
    } else {
        $Lines.Add("STAGE`t$Stage`tBLOB`t$Name`tMISSING")
    }
}

function Compare-Pair([System.Collections.Generic.List[string]]$Lines, [string]$Name, [string]$LeftPath, [string]$RightPath) {
    $Lines.Add("PAIR`t$Name")
    if (-not (Test-Path $LeftPath) -or -not (Test-Path $RightPath)) {
        $Lines.Add('MISSING')
        return
    }
    $left = [IO.File]::ReadAllBytes($LeftPath)
    $right = [IO.File]::ReadAllBytes($RightPath)
    $samePrefix = 0
    $n = [Math]::Min($left.Length, $right.Length)
    while ($samePrefix -lt $n -and $left[$samePrefix] -eq $right[$samePrefix]) { $samePrefix++ }
    $runs = Get-Runs $left $right
    $Lines.Add("SIZES`t$($left.Length)`t$($right.Length)`tSAME_PREFIX`t$samePrefix`tDIFFRUNS`t$($runs.Count)")
    $show = [Math]::Min(24, $runs.Count)
    for ($i = 0; $i -lt $show; $i++) {
        $r = $runs[$i]
        $Lines.Add("RUN`t$($r.Offset)`t$($r.Length)`t$($r.A)`t$($r.B)")
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('NTFS-EFS-PROBE-REPORT')
foreach ($s in @('Z', 'S', 'L', 'P', 'R')) {
    $ser = Join-Path $OutRoot "$s\guest-serial.log"
    $man = Join-Path $OutRoot "$s\captures\manifest.txt"
    if (Test-Path $ser) {
        $t = Get-Content $ser -Raw
        if ($t -match 'NTFS-EFS-PROBE-DONE\tok') { $lines.Add("STAGE`t$s`tSERIAL\tok") }
        else { $lines.Add("STAGE`t$s`tSERIAL\tfail") }
        if ($t -match 'FILE_ATTRIBUTE_ENCRYPTED set') { $lines.Add("STAGE`t$s`tATTR\tok") }
    } else {
        $lines.Add("STAGE`t$s`tSERIAL\tmissing")
    }
    if (Test-Path $man) {
        $mt = Get-Content $man -Raw
        if ($mt -match 'FOUND\tsecretA\t1') { $lines.Add("STAGE`t$s`tFOUND_A\t1") }
        if ($mt -match 'FOUND\tsecretB\t1') { $lines.Add("STAGE`t$s`tFOUND_B\t1") }
    }
    Add-BlobLines $lines $s 'secretA_logged_utility.bin'
    Add-BlobLines $lines $s 'secretA_data.bin'
    if ($s -eq 'P') {
        Add-BlobLines $lines $s 'secretB_logged_utility.bin'
        Add-BlobLines $lines $s 'secretB_data.bin'
    }
}

Compare-Pair $lines 'P-secretA-vs-secretB-efs' `
    (Join-Path $OutRoot 'P\captures\secretA_logged_utility.bin') `
    (Join-Path $OutRoot 'P\captures\secretB_logged_utility.bin')
Compare-Pair $lines 'S-vs-L-efs' `
    (Join-Path $OutRoot 'S\captures\secretA_logged_utility.bin') `
    (Join-Path $OutRoot 'L\captures\secretA_logged_utility.bin')
Compare-Pair $lines 'S-vs-Z-efs' `
    (Join-Path $OutRoot 'S\captures\secretA_logged_utility.bin') `
    (Join-Path $OutRoot 'Z\captures\secretA_logged_utility.bin')
Compare-Pair $lines 'S-vs-R-efs' `
    (Join-Path $OutRoot 'S\captures\secretA_logged_utility.bin') `
    (Join-Path $OutRoot 'R\captures\secretA_logged_utility.bin')
Compare-Pair $lines 'S-vs-L-data' `
    (Join-Path $OutRoot 'S\captures\secretA_data.bin') `
    (Join-Path $OutRoot 'L\captures\secretA_data.bin')

$lines.Add('NTFS-EFS-PROBE-REPORT-END')
[IO.File]::WriteAllLines($Report, $lines)
Write-Host "Wrote $Report"
Get-Content $Report
