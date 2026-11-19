<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-reparse.md
PURPOSE:     Reparse points, $REPARSE_POINT, and $Extend\$Reparse
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS reparse points

## Public / in-tree facts

[MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) lists `$Extend\$Reparse`.

[MS-FSCC: NTFS Stream Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702) names `$R` as `$Extend\$Reparse:$R:$INDEX_ALLOCATION`.

[MS-FSCC: NTFS Attribute Types](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85) lists `$REPARSE_POINT` (volume mount points among other uses).

[FSCTL_SET_REPARSE_POINT](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_reparse_point) sets a reparse point via `DeviceIoControl`.

In-tree:

- Attribute type `AttributeReparsePoint = 0xC0`.
- File attribute `NTFS_FILE_TYPE_REPARSE` (`0x400`).
- `REPARSE_DATA_BUFFER` and `REPARSE_DATA_BUFFER_HEADER_SIZE` in `sdk/include/xdk/iotypes.h` and `sdk/include/ndk/iotypes.h`.
- `NtfsSetReparsePoint` in `drivers/filesystems/ntfs/ntfs3x.c` validates buffer length against that header size and updates `$Reparse` under `$Extend`.
- `drivers/filesystems/ntfs/finfo.c` still has a FIXME to read the tag from `$REPARSE_POINT` for some info queries.
- Create path in `drivers/filesystems/ntfs/create.c` handles `FILE_OPEN_REPARSE_POINT` / mount-point tag `IO_REPARSE_TAG_MOUNT_POINT`.

## Compatibility outcomes

- Setting a reparse point stores a `$REPARSE_POINT` attribute and indexes `$Extend\$Reparse:$R` as implemented from the buffers above.
- Catalog: `Reparse`, `ReparsePoint`.

## Probe gates

- `$R` index key/value bytes beyond what `NtfsSetReparsePoint` already writes: capture before adding fields.
- Closing the finfo tag-read FIXME must use the in-tree attribute or a capture—not a guessed offset.

## Non-goals

- Implementing every reparse tag vendor under the sun; mount points and documented tags used by the suite come first.

### Resident attribute read / index-root insert bounds **(Rev.1)**

**(Rev.1)** In-tree `NtfsGetReparsePoint` and `NtfsGetReparseTag` (`drivers/filesystems/ntfs/ntfs3x.c`) copy using `Resident.ValueOffset` / `Resident.ValueLength` without requiring the attribute to be resident and the value range to sit inside the attribute record.

**(Rev.1)** Acceptance (**NTFS-ATTR-001**): require resident + in-record value bounds (types already in this tree’s headers) before copy.

**(Rev.1)** In-tree `NtfsInsertRootEntry` trusts disk index-root sizes then `RtlMoveMemory`s entries. Acceptance (**NTFS-IDX-001**): clamp header/`TotalSizeOfEntries` / entry chain to the resident value length and file-record bounds already typed in this tree before moving memory. No new Windows field names.

## Revision log

- **Rev.1** (2026-11-18): resident reparse read bounds (**NTFS-ATTR-001**); index-root insert clamp (**NTFS-IDX-001**).
