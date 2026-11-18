<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-volume-system-files.md
PURPOSE:     Boot sector, NTFS 3.1 volume info, and reserved system file names
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS volume and system files

## Public / in-tree facts

[MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) lists root metadata names including `$Mft`, `$MftMirr`, `$LogFile`, `$Volume`, `$AttrDef`, `$Bitmap`, `$Boot`, `$BadClus`, `$Secure`, `$UpCase`, and `$Extend`, plus `$Extend` children such as `$ObjId`, `$Quota`, `$Reparse`, and transactional names.

[MS-FSCC: NTFS Attribute Types](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85) names `$VOLUME_INFORMATION` (version and state) and `$VOLUME_NAME`.

[How NTFS Works](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc781134(v=ws.10)) assigns MFT record numbers 0–11 to those system files (and 12–15 reserved).

In-tree (`sdk/lib/fslib/ntfslib/ntfslib.h`, `files.c`):

- OEM id `"NTFS    "`, boot sector end `0x55AA`, `BOOT_SECTOR` / `EXTENDED_BIOS_PARAMETERS_BLOCK` with `$MFT` / `$MFTMirr` LCNs at EBPB offsets documented in that header.
- `NTFS_MAJOR_VERSION` 3, `NTFS_MINOR_VERSION` 1; `$VOLUME_INFORMATION` written 12 bytes with major/minor at offsets 8 and 9 and flags 0 (dirty clear).
- `STANDARD_INFORMATION` is 72 bytes for NTFS 3.x (`C_ASSERT` on size and key field offsets).
- Record numbers `METAFILE_*` / `NTFS_FILE_*` 0–11 and first user file 16 match the Learn table.
- `MFT_RECORD_SIZE` 1024; `FILE_RECORD_HEADER` layout declared in `ntfslib.h` and mirrored in `drivers/filesystems/ntfs/ntfs.h`.

## Compatibility outcomes

- Windows 10 22H2 must recognize the volume as NTFS 3.1 from boot + `$Volume`.
- System file names and record numbers above must exist after format.
- Catalog rows: BootSector, Version3_1, VolumeLabel, ClusterSize, Mft, LogFile, AttrDef, UpCase, Bitmap, BootFile, BadClus, Extend, FileRecordFixup, StandardInformation72.

## Probe gates

- Full `$AttrDef` table contents beyond the in-tree `ATTRIBUTES_TABLE` already emitted by `files.c`: if an implementer needs a row Windows writes that is not in that table, capture it—do not invent rows.
- Any boot-sector field not present in the in-tree `BOOT_SECTOR` structs: probe before adding.

## Non-goals

- Reconstructing layouts from WDK headers or symbol dumps.
- TxF (`$RmMetadata` / `$Txf*`) behavior beyond reserved names already listed in MS-FSCC (separate work if Milestone 45 requires it later).
