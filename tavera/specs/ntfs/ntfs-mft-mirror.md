<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-mft-mirror.md
PURPOSE:     $MFT and $MFTMirr roles for a Windows 10-compatible NTFS volume
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS `$MFT` and `$MFTMirr`

## Public / in-tree facts

[MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) lists `$Mft` as the master file table and `$MftMirr` as a backup copy of the first four records of the MFT.

[How NTFS Works](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc781134(v=ws.10)) says `$MftMirr` is a duplicate image of the first four records of the MFT and guarantees access if a single-sector failure hits the MFT.

In-tree:

- `MFT_MIRR_COUNT` is 4: `$MFT`, `$MFTMirr`, `$LogFile`, `$Volume` (`sdk/lib/fslib/ntfslib/ntfslib.h`).
- Format writes `$MFTMirr` as those first four records (`sdk/lib/fslib/ntfslib/files.c`).
- Boot EBPB stores `$MFT` and `$MFTMirr` locations (`EXTENDED_BIOS_PARAMETERS_BLOCK` in `ntfslib.h`).
- `UpdateMftMirror` in `drivers/filesystems/ntfs/mft.c` copies the first records from `$MFT` into `$MFTMirr`; the comment states the exact count follows `$MFTMirr` `$DATA` size (usually four) and that a stale mirror causes chkdsk to reject later MFT changes.

## Compatibility outcomes

- After format and after substantial MFT changes, `$MFTMirr` must match the first N records of `$MFT` where N is implied by the mirror `$DATA` length (four at format).
- Windows mount / chkdsk must not fail solely because the mirror was left stale after those records changed.
- Catalog: `Mft`, `MftMirror`.

## Probe gates

- If an implementer needs the precise rule Windows uses when `$MFTMirr` `$DATA` length is not exactly four records, capture that length and the copied bytes—do not invent N.

## Non-goals

- Copying Microsoft’s internal scheduling of when to refresh the mirror beyond keeping the first N records consistent with `$MFT`.
