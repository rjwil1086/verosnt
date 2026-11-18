<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-usn.md
PURPOSE:     USN change journal streams and USN_RECORD layout already in-tree
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS USN change journal

## Public / in-tree facts

[MS-FSCC: NTFS Stream Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702) names `$J` and `$MAX` on `$Extend\$UsnJrnl` (`$DATA`).

[Change Journal Records](https://learn.microsoft.com/en-us/windows/win32/fileio/change-journal-records) says NTFS appends change records; the offset of a record is its USN; records describe the fact and reason of a change, not enough to reverse it.

[USN_RECORD_V4](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_record_v4) documents reason flags and notes version 2/3 records as `USN_RECORD_V2` / `USN_RECORD` and `USN_RECORD_V3`.

In-tree:

- `CREATE_USN_JOURNAL_DATA` and `USN_RECORD` in `sdk/include/xdk/iotypes.h`.
- `FSCTL_CREATE_USN_JOURNAL`, read/query/delete paths wired in `drivers/filesystems/ntfs/fsctl.c`.
- Journal create/read/append in `drivers/filesystems/ntfs/ntfs3x.c` opens `$Extend\$UsnJrnl`, writes version 2 records (`MajorVersion = 2`, `MinorVersion = 0`) into `$J`, and grows the stream as needed.
- `STANDARD_INFORMATION.Usn` at offset `0x40` (72-byte SI in `ntfslib.h`).

## Compatibility outcomes

- Creating a journal and reading records must expose `USN_RECORD` fields consistent with the in-tree structure and Learn reason flags used by the suite.
- Catalog: `UsnJrnl`, `UsnJournal`.

## Probe gates

- If Windows requires journal metadata beyond `$MAX`/`$J` names and the in-tree `USN_RECORD` fields (for example a header Windows rejects), capture it before adding constants.
- Version 3/4 record support is out of scope until a capture or Learn+in-tree need appears.

## Non-goals

- Storing enough journal data to reverse changes (Learn states the journal does not).
