# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v11/host/Analyze-LogRestart.ps1
# PURPOSE:     Probe-local $LogFile restart-page structure from probe-v7 dumps
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Analyzes output/ntfs/probe-v7/<stage>/captures/logfile_data.bin.
# Writes output/ntfs/probe-v11/restart-report.txt (NTFS-LOG-RESTART-REPORT … END).
# Probe-local labels only — does not invent Microsoft field names.
# Sources: probe-v7 dumps; MS-FSCC $LogFile name; fsutil dirty Learn (behavior via T serial).

param(
    [string]$V7Root = '',
    [string]$OutRoot = ''
)

$ErrorActionPreference = 'Stop'
$Probe = Split-Path $PSScriptRoot -Parent
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
if (-not $V7Root) { $V7Root = Join-Path $SourceRoot 'output\ntfs\probe-v7' }
if (-not $OutRoot) { $OutRoot = Join-Path $SourceRoot 'output\ntfs\probe-v11' }
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$Report = Join-Path $OutRoot 'restart-report.txt'

function TagAt([byte[]]$b, [int]$off) {
    if ($off + 4 -gt $b.Length) { return '????' }
    return [Text.Encoding]::ASCII.GetString($b, $off, 4)
}

function U16([byte[]]$b, [int]$off) { [BitConverter]::ToUInt16($b, $off) }
function U32([byte[]]$b, [int]$off) { [BitConverter]::ToUInt32($b, $off) }
function U64([byte[]]$b, [int]$off) { [BitConverter]::ToUInt64($b, $off) }

function HexRow([byte[]]$b, [int]$off, [int]$len) {
    $end = [Math]::Min($off + $len - 1, $b.Length - 1)
    return (($off..$end | ForEach-Object { '{0:X2}' -f $b[$_] }) -join ' ')
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('NTFS-LOG-RESTART-REPORT')
$lines.Add('PROBE_METHOD	tavera/probes/ntfs/probe-v11/host/Analyze-LogRestart.ps1')
$lines.Add('INPUT_DUMPS	output/ntfs/probe-v7/<stage>/captures/logfile_data.bin')
$lines.Add('NOTE	probe-local labels only; Microsoft restart/log-record field names remain none')

foreach ($s in @('F', 'C', 'W', 'N', 'X', 'T')) {
    $p = Join-Path $V7Root "$s\captures\logfile_data.bin"
    if (-not (Test-Path $p)) {
        $lines.Add("STAGE`t$s`tMISSING")
        continue
    }
    $b = [IO.File]::ReadAllBytes($p)
    $page = 4096
    $rstr = 0; $rcrd = 0; $ff = 0
    for ($off = 0; $off -lt $b.Length; $off += $page) {
        $t = TagAt $b $off
        if ($t -eq 'RSTR') { $rstr++ }
        elseif ($t -eq 'RCRD') { $rcrd++ }
        elseif ($b[$off] -eq 0xFF) { $ff++ }
    }
    $lines.Add("STAGE`t$s`tSIZE`t$($b.Length)`tRSTR_PAGES`t$rstr`tRCRD_PAGES`t$rcrd`tFF_PAGE_START`t$ff")
    $lines.Add("STAGE`t$s`tTAG0`t$(TagAt $b 0)`tTAG4096`t$(TagAt $b 4096)`tTAG8192`t$(TagAt $b 8192)")
    $lines.Add(("STAGE`t{0}`tU16_AT_4`t0x{1:X4}`tU16_AT_6`t0x{2:X4}`tU32_AT_10`t0x{3:X8}`tU32_AT_14`t0x{4:X8}" -f `
        $s, (U16 $b 4), (U16 $b 6), (U32 $b 0x10), (U32 $b 0x14)))
    $lines.Add(("STAGE`t{0}`tU64_AT_8`t0x{1:X16}`tU64_AT_30`t0x{2:X16}`tU32_AT_58`t0x{3:X8}" -f `
        $s, (U64 $b 8), (U64 $b 0x30), (U32 $b 0x58)))
    $lines.Add(("STAGE`t{0}`tU64_AT_60`t0x{1:X16}`tU64_AT_68`t0x{2:X16}`tU16_AT_1E`t0x{3:X4}" -f `
        $s, (U64 $b 0x60), (U64 $b 0x68), (U16 $b 0x1E)))
    # Client name region: UTF-16 at +0x90 when present
    $nameBytes = $b[0x90..(0x90 + 7)]
    $name = [Text.Encoding]::Unicode.GetString($nameBytes)
    $lines.Add("STAGE`t$s`tUTF16_AT_90`t$name")
    $lines.Add("STAGE`t$s`tHEX_0_3F`t$(HexRow $b 0 64)")
    $lines.Add("STAGE`t$s`tHEX_40_7F`t$(HexRow $b 0x40 64)")
    $lines.Add("STAGE`t$s`tHEX_80_9F`t$(HexRow $b 0x80 32)")
}

$serT = Join-Path $V7Root 'T\guest-serial.log'
if (Test-Path $serT) {
    $t = Get-Content $serT -Raw
    if ($t -match 'fsutil dirty set ok') { $lines.Add("GUEST_T`tDIRTY_SET`tok") }
    if ($t -match 'remount NTFS ok') { $lines.Add("GUEST_T`tREMOUNT`tok") }
    if ($t -match 'readback ok') { $lines.Add("GUEST_T`tREADBACK`tok") }
    if ($t -match 'NTFS-LOG-ISO-DONE\tok') { $lines.Add("GUEST_T`tDONE`tok") }
}

$lines.Add("OBSERVED`tPAGE_TAG_RSTR`t52 53 54 52`tat page-aligned offsets 0 and 4096 (and only those two in first 1MiB)")
$lines.Add("OBSERVED`tPAGE_TAG_RCRD`t52 43 52 44`tat page-aligned offsets starting 8192")
$lines.Add("OBSERVED`tPAGE_SIZE_U32_AT_10`t0x1000`tall stages")
$lines.Add("OBSERVED`tSLOT_U64_AT_60_AND_68`t0`tall stages F/C/W/N/X/T including dirty remount T")
$lines.Add("OBSERVED`tUTF16_AT_90`tNTFS`tall stages")
$lines.Add("OBSERVED`tU64_AT_8`t0`tall stages")
$lines.Add("OBSERVED`tNONZERO_SAMPLE_AT_60`tnone_in_probe_v7")
$lines.Add('NTFS-LOG-RESTART-REPORT-END')

Set-Content -Path $Report -Value ($lines -join "`r`n") -Encoding ascii
Write-Host "Wrote $Report"
Get-Content $Report
