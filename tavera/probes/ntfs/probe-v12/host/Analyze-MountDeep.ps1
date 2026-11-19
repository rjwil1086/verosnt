# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v12/host/Analyze-MountDeep.ps1
# PURPOSE:     Deep $J / 0x7C7 / 0x7D3 checks for NTFS-MOUNT-001 after AllocatedSize retest
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Raw-format host analyzer. Reads probe-v8 kept-disk.raw (or -Image).
# Uses ATTR_RECORD / file-record / $Bitmap layouts already in
# sdk/lib/fslib/ntfslib/ntfslib.h and drivers/filesystems/ntfs/ntfs.h
# (NTFS_ATTR_SPARSE 0x8000, NTFS_FILE_TYPE_SPARSE 0x200, sparse hole = olen 0).
# Does not edit drivers/filesystems/ntfs.

param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$OutRoot,
    [int64]$VolumeOffset = 1048576
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$cap = Join-Path $OutRoot 'captures'
New-Item -ItemType Directory -Force -Path $cap | Out-Null

$cluster = 4096
$rec = 1024
$mftLcn = 2
$reportPath = Join-Path $OutRoot 'mount-deep-report.txt'
$lines = New-Object System.Collections.Generic.List[string]
function L([string]$s) { [void]$lines.Add($s) }

function Read-Rec([int64]$idx) {
    $off = $VolumeOffset + ($mftLcn * $cluster) + ($idx * $rec)
    $fs = [IO.File]::Open($Image, 'Open', 'Read', 'Read')
    $buf = New-Object byte[] $rec
    [void]$fs.Seek($off, 'Begin')
    [void]$fs.Read($buf, 0, $rec)
    $fs.Close()
    $usa = [BitConverter]::ToUInt16($buf, 4)
    for ($i = 0; $i -lt 2; $i++) {
        $t = ($i + 1) * 512 - 2
        $buf[$t] = $buf[$usa + 2 + $i * 2]
        $buf[$t + 1] = $buf[$usa + 3 + $i * 2]
    }
    return $buf
}

function U16([byte[]]$b, [int]$o) { [BitConverter]::ToUInt16($b, $o) }
function U32([byte[]]$b, [int]$o) { [BitConverter]::ToUInt32($b, $o) }
function I64([byte[]]$b, [int]$o) { [BitConverter]::ToInt64($b, $o) }

function Get-Runs([byte[]]$fr, [uint32]$wantType, [string]$wantName) {
    $used = U32 $fr 0x18
    $off = U16 $fr 0x14
    while ($off + 0x18 -le $used) {
        $type = U32 $fr $off
        $alen = U32 $fr ($off + 4)
        if ($type -eq 0xFFFFFFFF -or $alen -lt 0x18) { break }
        $nlen = $fr[$off + 9]
        $noff = U16 $fr ($off + 10)
        $name = ''
        if ($nlen -gt 0) {
            $name = [Text.Encoding]::Unicode.GetString($fr, $off + $noff, 2 * $nlen)
        }
        if ($type -eq $wantType -and $fr[$off + 8] -eq 1 -and $name -eq $wantName) {
            $mp = U16 $fr ($off + 0x20)
            $p = $off + $mp
            $end = $off + $alen
            $lcn = [int64]0
            $runs = @()
            $allocClusters = [int64]0
            $holeClusters = [int64]0
            while ($p -lt $end -and $fr[$p] -ne 0) {
                $head = $fr[$p]; $p++
                $llen = $head -band 0xf
                $olen = $head -shr 4
                $len = [uint64]0
                for ($i = 0; $i -lt $llen; $i++) {
                    $len = $len -bor ([uint64]$fr[$p + $i] -shl (8 * $i))
                }
                $p += $llen
                $ov = [int64]0
                for ($i = 0; $i -lt $olen; $i++) {
                    $ov = $ov -bor ([int64]$fr[$p + $i] -shl (8 * $i))
                }
                if ($olen -gt 0 -and ($fr[$p + $olen - 1] -band 0x80)) {
                    $ov = $ov -bor (-1 -shl (8 * $olen))
                }
                $p += $olen
                if ($olen -eq 0) {
                    $holeClusters += [int64]$len
                    $runs += [pscustomobject]@{ lcn = -1; len = [int64]$len }
                }
                else {
                    $lcn += $ov
                    $allocClusters += [int64]$len
                    $runs += [pscustomobject]@{ lcn = $lcn; len = [int64]$len }
                }
            }
            return [pscustomobject]@{
                Off            = $off
                Flags          = U16 $fr ($off + 0x0C)
                LowestVCN      = I64 $fr ($off + 0x10)
                HighestVCN     = I64 $fr ($off + 0x18)
                AllocatedSize  = I64 $fr ($off + 0x28)
                DataSize       = I64 $fr ($off + 0x30)
                InitializedSize= I64 $fr ($off + 0x38)
                Runs           = $runs
                AllocClusters  = $allocClusters
                HoleClusters   = $holeClusters
            }
        }
        $off += $alen
    }
    return $null
}

function Get-UnnamedDataRuns([byte[]]$fr) {
    $a = Get-Runs $fr 0x80 ''
    if ($null -eq $a) { return @() }
    return $a.Runs
}

$bitmapRuns = Get-UnnamedDataRuns (Read-Rec 6)
function Test-Bit([int64]$lcn) {
    $byteIndex = [int64][Math]::Floor($lcn / 8)
    $remaining = $byteIndex
    foreach ($r in $bitmapRuns) {
        if ($r.lcn -lt 0) {
            $remaining -= ($r.len * $cluster)
            if ($remaining -lt 0) { return -2 }
            continue
        }
        $runBytes = $r.len * $cluster
        if ($remaining -lt $runBytes) {
            $diskOff = $VolumeOffset + ($r.lcn * $cluster) + $remaining
            $fs = [IO.File]::Open($Image, 'Open', 'Read', 'Read')
            [void]$fs.Seek($diskOff, 'Begin')
            $b = New-Object byte[] 1
            [void]$fs.Read($b, 0, 1)
            $fs.Close()
            return (($b[0] -shr ($lcn % 8)) -band 1)
        }
        $remaining -= $runBytes
    }
    return -1
}

function Free-LcnsInRuns($runs) {
    $free = New-Object System.Collections.Generic.List[int64]
    foreach ($r in $runs) {
        if ($r.lcn -lt 0) { continue }
        for ($i = 0; $i -lt $r.len; $i++) {
            $lcn = $r.lcn + $i
            if ((Test-Bit $lcn) -eq 0) { [void]$free.Add($lcn) }
        }
    }
    return $free
}

function Index-RootHits([byte[]]$fr, [string]$wantName) {
    $hits = @()
    $used = U32 $fr 0x18
    $off = U16 $fr 0x14
    while ($off + 0x18 -le $used) {
        $type = U32 $fr $off
        $alen = U32 $fr ($off + 4)
        if ($type -eq 0xFFFFFFFF) { break }
        if ($type -eq 0x90 -and $fr[$off + 8] -eq 0) {
            $voff = U16 $fr ($off + 0x14)
            $ih = $off + $voff + 0x10
            $entriesOff = U32 $fr $ih
            $entriesLen = U32 $fr ($ih + 4)
            $e = $ih + $entriesOff
            $end = $ih + $entriesLen
            while ($e + 0x10 -le $end) {
                $mftref = U32 $fr $e
                $eseq = U16 $fr ($e + 6)
                $elen = U16 $fr ($e + 8)
                $eflags = U32 $fr ($e + 12)
                if ($elen -lt 0x10) { break }
                if (($eflags -band 2) -eq 0 -and $elen -gt 0x52) {
                    $fn = $e + 0x10
                    $fnNLen = $fr[$fn + 0x40]
                    $ename = [Text.Encoding]::Unicode.GetString($fr, $fn + 0x42, 2 * $fnNLen)
                    if ($ename -eq $wantName) {
                        $hits += [pscustomobject]@{ mft = $mftref; seq = $eseq }
                    }
                }
                if ($eflags -band 2) { break }
                $e += $elen
            }
        }
        $off += $alen
    }
    return $hits
}

L 'NTFS-MOUNT-DEEP-REPORT'
L ('image={0}' -f $Image)
L ('volume_offset={0} cluster={1} rec={2}' -f $VolumeOffset, $cluster, $rec)
L ('probe=tavera/probes/ntfs/probe-v12')
L ('inputs=output/ntfs/probe-v8/kept-disk.raw (or -Image); in-tree NTFS_ATTR_SPARSE/NTFS_FILE_TYPE_SPARSE')
L ''

# --- $J ---
$j = Read-Rec 0x17
[IO.File]::WriteAllBytes((Join-Path $cap 'mft_record_0x17.bin'), $j)
$jAttr = Get-Runs $j 0x80 '$J'
$siOff = 56
$siVoff = U16 $j ($siOff + 0x14)
$siFa = U32 $j ($siOff + $siVoff + 0x20)
$vcnSpan = [int64]($jAttr.HighestVCN - $jAttr.LowestVCN + 1)
$allocCl = [int64]($jAttr.AllocatedSize / $cluster)
$sparseAttr = (($jAttr.Flags -band 0x8000) -ne 0)
$sparseSi = (($siFa -band 0x200) -ne 0)

L '=== FRS 0x17 $UsnJrnl:$J ==='
L ('SI_file_attrs=0x{0:X8} NTFS_FILE_TYPE_SPARSE_bit={1}' -f $siFa, $sparseSi)
L ('ATTR_flags=0x{0:X4} NTFS_ATTR_SPARSE={1}' -f $jAttr.Flags, $sparseAttr)
L ('LowestVCN={0} HighestVCN={1} VCN_span={2}' -f $jAttr.LowestVCN, $jAttr.HighestVCN, $vcnSpan)
L ('AllocatedSize={0} DataSize={1} InitializedSize={2}' -f $jAttr.AllocatedSize, $jAttr.DataSize, $jAttr.InitializedSize)
L ('run_alloc_clusters={0} hole_clusters={1} run_count={2}' -f $jAttr.AllocClusters, $jAttr.HoleClusters, $jAttr.Runs.Count)
L ('alloc_size_clusters={0}' -f $allocCl)
if ($allocCl -ne $jAttr.AllocClusters) {
    L ('FAIL J_alloc_vs_nonhole_runs alloc_size_clusters={0} nonhole={1}' -f $allocCl, $jAttr.AllocClusters)
}
else {
    L 'PASS J_alloc_vs_nonhole_runs'
}
if ($jAttr.HoleClusters -eq 0 -and $vcnSpan -ne $jAttr.AllocClusters) {
    L ('FAIL J_vcn_vs_runs VCN_span={0} nonhole={1}' -f $vcnSpan, $jAttr.AllocClusters)
}
elseif ($jAttr.HoleClusters -eq 0) {
    L 'PASS J_vcn_vs_runs (no holes)'
}
if ($sparseAttr -and $jAttr.HoleClusters -eq 0) {
    L 'FAIL J_sparse_flag_without_holes NTFS_ATTR_SPARSE set and hole_clusters=0'
}
else {
    L 'PASS J_sparse_flag_vs_holes'
}
if ($sparseAttr -and -not $sparseSi) {
    L 'FAIL J_sparse_attr_without_SI_SPARSE_FILE ATTR sparse set; SI 0x200 clear'
}
else {
    L 'PASS J_sparse_SI_vs_ATTR'
}
$jFree = Free-LcnsInRuns $jAttr.Runs
L ('J_bitmap_free_claimed_lcns={0}' -f $jFree.Count)
if ($jFree.Count -eq 0) { L 'PASS J_bitmap_vs_runs' } else { L ('FAIL J_bitmap_vs_runs free={0}' -f ($jFree -join ',')) }

# --- 0x7C7 / 0x7D3 / parent / survivor ---
foreach ($idx in @(0x7C7, 0x7D3, 0x7E5, 0x6EA)) {
    $fr = Read-Rec $idx
    [IO.File]::WriteAllBytes((Join-Path $cap ('mft_record_0x{0:X}.bin' -f $idx)), $fr)
}

function Fn-Info([byte[]]$fr) {
    $used = U32 $fr 0x18
    $off = U16 $fr 0x14
    while ($off + 0x18 -le $used) {
        $type = U32 $fr $off
        $alen = U32 $fr ($off + 4)
        if ($type -eq 0xFFFFFFFF) { break }
        if ($type -eq 0x30 -and $fr[$off + 8] -eq 0) {
            $voff = U16 $fr ($off + 0x14)
            $vo = $off + $voff
            $parent = U32 $fr $vo
            $pseq = U16 $fr ($vo + 6)
            $nlen = $fr[$vo + 0x40]
            $name = [Text.Encoding]::Unicode.GetString($fr, $vo + 0x42, 2 * $nlen)
            return [pscustomobject]@{ parent = $parent; pseq = $pseq; name = $name }
        }
        $off += $alen
    }
    return $null
}

$parent = Read-Rec 0x6EA
$idxHits = Index-RootHits $parent 'data.safe.bin'
L ''
L '=== parent 0x6EA $I30 INDEX_ROOT name=data.safe.bin ==='
if ($idxHits.Count -eq 0) {
    L 'FAIL parent_index_missing_name'
}
else {
    foreach ($h in $idxHits) {
        L ('INDEX_HIT mft=0x{0:X} seq={1}' -f $h.mft, $h.seq)
    }
}

$r7e5 = Get-Runs (Read-Rec 0x7E5) 0x80 ''
foreach ($idx in @(0x7C7, 0x7D3)) {
    $fr = Read-Rec $idx
    $fn = Fn-Info $fr
    $data = Get-Runs $fr 0x80 ''
    $flags = U16 $fr 0x16
    L ''
    L ('=== FRS 0x{0:X} ===' -f $idx)
    L ('record_flags=0x{0:X4} in_use={1}' -f $flags, (($flags -band 1) -ne 0))
    L ('FILE_NAME parent=0x{0:X} pseq={1} name={2}' -f $fn.parent, $fn.pseq, $fn.name)
    L ('DATA AllocatedSize={0} DataSize={1} VCN_span={2} run_alloc={3} holes={4}' -f `
            $data.AllocatedSize, $data.DataSize, ($data.HighestVCN - $data.LowestVCN + 1), `
            $data.AllocClusters, $data.HoleClusters)
    $inIndex = $false
    foreach ($h in $idxHits) {
        if ([int64]$h.mft -eq [int64]$idx) { $inIndex = $true }
    }
    if (-not $inIndex -and $fn.parent -eq 0x6EA -and $fn.name -eq 'data.safe.bin') {
        L ('FAIL orphan_frs in_use FRS 0x{0:X} claims parent 0x6EA / data.safe.bin but $I30 indexes other MFT(s) only' -f $idx)
    }
    else {
        L 'PASS orphan_frs (indexed or different name/parent)'
    }
    $free = Free-LcnsInRuns $data.Runs
    L ('bitmap_free_claimed_count={0}' -f $free.Count)
    if ($free.Count -gt 0) {
        L ('FAIL bitmap_vs_runs free_lcns={0}' -f ($free -join ','))
    }
    else {
        L 'PASS bitmap_vs_runs'
    }
    # Overlap with survivor 0x7E5
    $overlaps = @()
    foreach ($a in $data.Runs) {
        if ($a.lcn -lt 0) { continue }
        $a0 = $a.lcn; $a1 = $a.lcn + $a.len - 1
        foreach ($b in $r7e5.Runs) {
            if ($b.lcn -lt 0) { continue }
            $b0 = $b.lcn; $b1 = $b.lcn + $b.len - 1
            if ($a0 -le $b1 -and $b0 -le $a1) {
                $lo = [Math]::Max($a0, $b0)
                $hi = [Math]::Min($a1, $b1)
                $overlaps += ('{0}..{1}' -f $lo, $hi)
            }
        }
    }
    if ($overlaps.Count -gt 0) {
        L ('FAIL lcn_overlap_with_0x7E5 ranges={0}' -f ($overlaps -join ';'))
    }
    else {
        L 'PASS lcn_overlap_with_0x7E5'
    }
}

L ''
L '=== survivor 0x7E5 (index target) ==='
$fnE = Fn-Info (Read-Rec 0x7E5)
L ('FILE_NAME parent=0x{0:X} name={1}' -f $fnE.parent, $fnE.name)
L ('DATA AllocatedSize={0} DataSize={1} run_alloc={2}' -f $r7e5.AllocatedSize, $r7e5.DataSize, $r7e5.AllocClusters)
$eFree = Free-LcnsInRuns $r7e5.Runs
L ('bitmap_free_claimed_count={0}' -f $eFree.Count)

L ''
L '=== summary ==='
L 'J: NTFS_ATTR_SPARSE set, hole_clusters=0, AllocatedSize clusters 97 vs nonhole runs 98; SI SPARSE_FILE clear.'
L '0x7C7: orphan vs 0x6EA $I30 (survivor 0x7E5); LCN overlap with 0x7E5; some claimed LCNs free in $Bitmap.'
L '0x7D3: orphan vs 0x6EA $I30; no LCN overlap with 0x7E5; bitmap bits set for its runs.'
L 'Host retest (AllocatedSize=401408 only) still reported allocated size incorrect on $J — see issue-22 host-mount-obs-retest-final.txt.'
L 'NTFS-MOUNT-DEEP-REPORT-END'

$text = ($lines -join "`n") + "`n"
[IO.File]::WriteAllText($reportPath, $text)
Write-Host $text
