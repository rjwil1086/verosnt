# PROJECT: VerOS NT
# LICENSE: GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE: tavera/test-suites/ntfs/Get-NtfsBytes.ps1
# PURPOSE: Print comparable NTFS file-record fields
# COPYRIGHT: Copyright 2026 Ryan Wilson <ryan@verosnt.com>
# PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>

param([Parameter(Mandatory = $true)][string]$Image)

function U16([int]$o) { return [uint16]([int]$script:B[$o] -bor ([int]$script:B[$o + 1] -shl 8)) }
function U32([int]$o) {
    return [uint32]([int]$script:B[$o] -bor ([int]$script:B[$o + 1] -shl 8) -bor ([int]$script:B[$o + 2] -shl 16) -bor ([int]$script:B[$o + 3] -shl 24))
}
function U64([int]$o) {
    return [uint64](U32 $o) -bor ([uint64](U32 ($o + 4)) -shl 32)
}

function Apply-Fixup([byte[]]$rec, [int]$recBytes, [int]$sector) {
    $script:B = $rec
    $usaOff = U16 4
    $usaCount = U16 6
    if ($sector -lt 512 -or ($recBytes % $sector) -ne 0) { return $false }
    $sectors = [int]($recBytes / $sector)
    if ($usaCount -ne ($sectors + 1)) { return $false }
    $usn = U16 $usaOff
    for ($i = 0; $i -lt $sectors; $i++) {
        $tail = ($i + 1) * $sector - 2
        if ((U16 $tail) -ne $usn) { return $false }
        $rec[$tail] = $rec[$usaOff + 2 + $i * 2]
        $rec[$tail + 1] = $rec[$usaOff + 3 + $i * 2]
    }
    return $true
}

$img = [IO.File]::ReadAllBytes($Image)
$script:B = $img
$vol = [uint64]0
$oem = [Text.Encoding]::ASCII.GetString($img, 3, 8)
if ($oem -ne 'NTFS    ') {
    for ($part = 0; $part -lt 4; $part++) {
        $ent = 0x1BE + $part * 16
        if ($img[$ent + 4] -eq 0x07) { $vol = [uint64](U32 ($ent + 8)) * 512; break }
    }
}
$script:B = $img
$base = [int]$vol
$bps = U16 ($base + 0x0B)
$spc = $img[$base + 0x0D]
$cluster = [uint32]$bps * [uint32]$spc
$sectors = U64 ($base + 0x28)
$mftLcn = U64 ($base + 0x30)
$cprRaw = [int]$img[$base + 0x40]
if ($cprRaw -gt 127) { $cpr = $cprRaw - 256 } else { $cpr = $cprRaw }
if ($cpr -gt 0) { $recBytes = [int]$cpr * [int]$cluster } else { $recBytes = 1 -shl (-$cpr) }

function Slice([uint64]$pos, [int]$len) {
    $out = New-Object byte[] $len
    [Array]::Copy($img, [int64]$pos, $out, 0, $len)
    return $out
}

$rec0 = Slice ($vol + $mftLcn * $cluster) $recBytes
if (-not (Apply-Fixup $rec0 $recBytes $bps)) { throw 'MFT fixup failed' }
$off = U16 0x14
$used = U32 0x18
$lcns = New-Object System.Collections.Generic.List[int64]
$lens = New-Object System.Collections.Generic.List[uint64]
while ($off + 8 -le $used) {
    $type = U32 $off
    $len = U32 ($off + 4)
    if ($type -eq 0xffffffff -or $len -lt 24) { break }
    if ($rec0[$off + 8] -eq 1 -and $type -eq 0x80 -and $rec0[$off + 9] -eq 0) {
        $map = U16 ($off + 0x20)
        $p = $off + $map
        $end = $off + $len
        $lcn = [int64]0
        while ($p -lt $end -and $rec0[$p] -ne 0) {
            $head = $rec0[$p]; $p++
            $llen = $head -band 0x0f
            $olen = $head -shr 4
            $runlen = [uint64]0
            for ($i = 0; $i -lt $llen; $i++) { $runlen = $runlen -bor ([uint64]$rec0[$p] -shl (8 * $i)); $p++ }
            if ($olen -eq 0) { $lcn = -1 }
            else {
                $delta = [int64]0
                for ($i = 0; $i -lt $olen; $i++) { $delta = $delta -bor ([int64]$rec0[$p] -shl (8 * $i)); $p++ }
                if ($olen -lt 8 -and ($rec0[$p - 1] -band 0x80)) {
                    for ($b = $olen; $b -lt 8; $b++) { $delta = $delta -bor ([int64]0xff -shl (8 * $b)) }
                }
                $lcn += $delta
            }
            $lcns.Add($lcn) | Out-Null
            $lens.Add($runlen) | Out-Null
        }
        break
    }
    $off += $len
}

function Read-Record([uint64]$index) {
    $skip = $index * [uint64]$recBytes
    $filled = 0
    $out = New-Object byte[] $recBytes
    for ($i = 0; $i -lt $lcns.Count -and $filled -lt $recBytes; $i++) {
        $bytes = $lens[$i] * [uint64]$cluster
        if ($bytes -eq 0) { return $null }
        if ($skip -ge $bytes) { $skip -= $bytes; continue }
        if ($lcns[$i] -lt 0) { return $null }
        while ($filled -lt $recBytes -and $skip -lt $bytes) {
            $pos = $vol + [uint64]$lcns[$i] * $cluster + $skip
            $take = $recBytes - $filled
            if ($take -gt ($bytes - $skip)) { $take = [int]($bytes - $skip) }
            [Array]::Copy($img, [int64]$pos, $out, $filled, $take)
            $filled += $take
            $skip += [uint64]$take
        }
        $skip = 0
    }
    if ($filled -ne $recBytes) { return $null }
    return $out
}

$rows = @{}
$major = 0; $minor = 0
for ($i = 0; $i -lt 2048; $i++) {
    $rec = Read-Record $i
    if ($null -eq $rec) { break }
    if ([Text.Encoding]::ASCII.GetString($rec, 0, 4) -ne 'FILE') { continue }
    if (-not (Apply-Fixup $rec $recBytes $bps)) { continue }
    if (((U16 0x16) -band 1) -eq 0) { continue }
    if ($i -eq 3) {
        $a = U16 0x14
        $u = U32 0x18
        while ($a + 8 -le $u) {
            $type = U32 $a
            $len = U32 ($a + 4)
            if ($type -eq 0xffffffff -or $len -lt 24) { break }
            if ($type -eq 0x70 -and $rec[$a + 8] -eq 0) {
                $voff = U16 ($a + 0x14)
                $major = $rec[$a + $voff + 8]
                $minor = $rec[$a + $voff + 9]
            }
            $a += $len
        }
    }
    if ($i -lt 16) { continue }
    $a = U16 0x14
    $u = U32 0x18
    $names = @()
    $si = 0; $list = 0; $obj = 0; $efs = 0; $hole = 0; $data = 'none'; $cu = 0; $reparse = 0; $fileattr = 0
    while ($a + 8 -le $u) {
        $type = U32 $a
        $len = U32 ($a + 4)
        if ($type -eq 0xffffffff -or $len -lt 24 -or ($a + $len) -gt $u) { break }
        $nonres = $rec[$a + 8]
        $nlen = $rec[$a + 9]
        if ($type -eq 0x10 -and $nonres -eq 0) {
            $si = U32 ($a + 0x10)
            $voff = U16 ($a + 0x14)
            if ($voff + 36 -le $len) { $fileattr = U32 ($a + $voff + 0x20) }
        } elseif ($type -eq 0x20) { $list = 1 }
        elseif ($type -eq 0x40) { $obj = 1 }
        elseif ($type -eq 0xC0 -and $nonres -eq 0) {
            $voff = U16 ($a + 0x14)
            if ($voff + 4 -le $len) { $reparse = U32 ($a + $voff) }
        } elseif ($type -eq 0x100 -and $nlen -eq 4) {
            $noff = U16 ($a + 10)
            if ($noff + 8 -le $len -and (U16 ($a + $noff)) -eq 0x24 -and (U16 ($a + $noff + 2)) -eq 0x45) { $efs = 1 }
        } elseif ($type -eq 0x30 -and $nonres -eq 0) {
            $vlen = U32 ($a + 0x10)
            $voff = U16 ($a + 0x14)
            if ($vlen -ge 0x42) {
                $val = $a + $voff
                $chars = $rec[$val + 0x40]
                $text = ''
                for ($c = 0; $c -lt $chars; $c++) {
                    $ch = U16 ($val + 0x42 + $c * 2)
                    if ($ch -ge 32 -and $ch -lt 127) { $text += [char]$ch } else { $text += '?' }
                }
                $pref = U64 $val
                $names += [pscustomobject]@{
                    Type = $rec[$val + 0x41]
                    Parent = $pref -band 0x0000ffffffffffffL
                    Pseq = [uint16]($pref -shr 48)
                    Text = $text
                }
            }
        } elseif ($type -eq 0x80 -and $nlen -eq 0) {
            if ($nonres -eq 0) { $data = 'R' }
            else {
                $data = 'NR'
                $cu = U16 ($a + 0x22)
                $map = U16 ($a + 0x20)
                $p = $a + $map
                $end = $a + $len
                while ($p -lt $end -and $rec[$p] -ne 0) {
                    $head = $rec[$p]; $p++
                    $llen = $head -band 0x0f
                    $olen = $head -shr 4
                    if ($olen -eq 0) { $hole = 1 }
                    $p += $llen + $olen
                }
            }
        }
        $a += $len
    }
    $pick = $null
    foreach ($n in $names) {
        if ($null -eq $pick) { $pick = $n; continue }
        $win = ($n.Type -eq 1 -or $n.Type -eq 3)
        $pickWin = ($pick.Type -eq 1 -or $pick.Type -eq 3)
        if ($win -and -not $pickWin) { $pick = $n }
    }
    if ($null -eq $pick -or $pick.Text.StartsWith('$')) { continue }
    $sorted = $names | Sort-Object Type, Text
    $nameText = ($sorted | ForEach-Object {
        $kind = switch ($_.Type) { 0 { 'POSIX' } 1 { 'WIN32' } 2 { 'DOS' } 3 { 'WIN32DOS' } default { 'OTHER' } }
        "$kind`:$($_.Parent).$($_.Pseq):$($_.Text)"
    }) -join ';'
    $rows[$i] = [pscustomobject]@{
        Leaf = $pick.Text
        Parent = [uint64]$pick.Parent
        Rest = "si=$si|dir=$(((U16 0x16) -band 2) -ne 0)|comp=$(($fileattr -band 0x800) -ne 0)|enc=$(($fileattr -band 0x4000) -ne 0)|sparse=$(($fileattr -band 0x200) -ne 0)|reparse=$reparse|obj=$obj|efs=$efs|list=$list|data=$data|cu=$cu|hole=$hole|names=$nameText"
    }
}

Write-Output ("VOL ver={0}.{1} cluster={2}" -f $major, $minor, $cluster)
foreach ($i in ($rows.Keys | Sort-Object)) {
    $path = ''
    $cur = [uint64]$i
    $guard = 0
    while ($cur -ne 5 -and $rows.ContainsKey([int]$cur) -and $guard -lt 12) {
        if ($path -eq '') { $path = $rows[[int]$cur].Leaf } else { $path = $rows[[int]$cur].Leaf + '\' + $path }
        if ($rows[[int]$cur].Parent -eq $cur) { break }
        $cur = $rows[[int]$cur].Parent
        $guard++
    }
    if ($path) { Write-Output ("FILE {0}|{1}" -f $path, $rows[[int]$i].Rest) }
}
