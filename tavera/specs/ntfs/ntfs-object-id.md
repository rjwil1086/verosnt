<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-object-id.md
PURPOSE:     Object ID FSCTL buffers and $Extend\$ObjId for NTFS 3.1
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS object IDs

## Public / in-tree facts

[MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) lists `$Extend\$ObjId` as unique ids given to files.

[MS-FSCC: NTFS Stream Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702) names `$O` as `$Extend\$ObjId:$O:$INDEX_ALLOCATION`.

[MS-FSCC: NTFS Attribute Types](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85) lists `$OBJECT_ID` as a unique GUID for every MFT record.

[FSCTL_CREATE_OR_GET_OBJECT_ID](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_create_or_get_object_id) retrieves an object id and creates one if missing. [MS-FSCC reply](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/75f02ca0-2c62-42a3-a2c1-a591ef0bdb5a) returns a `FILE_OBJECTID_BUFFER`. [MS-FSA](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/4f329bbc-eacf-4839-9e46-dd7780759132) gives status rules including `STATUS_VOLUME_NOT_UPGRADED` when object ids are unsupported.

In-tree:

- Attribute type `AttributeObjectId = 0x40` (`ntfs.h` / `ntfslib.h`).
- `FILE_OBJECTID_BUFFER` in `sdk/include/xdk/iotypes.h` (`ObjectId[16]` plus birth/domain fields).
- `NtfsQueryObjectId` and `$ObjId` extend lookup in `drivers/filesystems/ntfs/ntfs3x.c`.

## Compatibility outcomes

- `FSCTL_CREATE_OR_GET_OBJECT_ID` / get paths return a `FILE_OBJECTID_BUFFER` sized and fielded as in-tree / MS-FSCC.
- Catalog: `ObjId`, `ObjectIdApi`.

## Probe gates

- Bytes stored as the `$O` index entry value (beyond the GUID / buffer fields already in `FILE_OBJECTID_BUFFER`) need a capture before new constants are added.

## Non-goals

- Applications modifying object ids (Learn warns that modification can lose data).

### Resident object-id read bounds **(Rev.1)**

**(Rev.1)** In-tree `NtfsQueryObjectId` (`drivers/filesystems/ntfs/ntfs3x.c`) uses resident value offset/length for copies. Acceptance (**NTFS-ATTR-001**): same resident + in-record clamp as reparse reads before copy. No new Windows field names.

## Revision log

- **Rev.1** (2026-11-18): resident object-id read bounds; **NTFS-ATTR-001**.
