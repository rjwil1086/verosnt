<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-read-write-remount.md
PURPOSE:     Read, write, and remount persistence acceptance for NTFS 3.1
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS read, write, and remount

## Public / in-tree facts

[MS-FSCC: NTFS Attribute Types](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85) lists `$DATA` as the default file data attribute and `$STANDARD_INFORMATION` for times and related fields.

CreateFile / ReadFile / WriteFile / SetEndOfFile behavior for ordinary files is the documented Win32 contract; this spec does not restate every status code. Compatibility is judged by those APIs and by Windows mounting the resulting volume ([`ntfs-windows-clean-mount.md`](ntfs-windows-clean-mount.md)).

In-tree:

- Resident and non-resident `$DATA` via `ATTR_RECORD` / `NTFS_ATTR_RECORD` (`ntfslib.h`, `ntfs.h`).
- Read/write paths in `drivers/filesystems/ntfs/rw.c` (encrypted path delegates to stubs documented in [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md)).
- Create/delete/rename in `drivers/filesystems/ntfs/create.c` and related files.
- Suite catalog rows exercised by `unittests/ntfs/agent/ntfs_agent.c` (buildenv): ResidentData, NonResidentData, EndOfFile, AllocationSize, Create, Delete, Rename, Remount, Timestamps, AlternateStream, HardLink, DirectoryIndex, …

## Compatibility outcomes

- Data written through documented APIs must read back equal bytes after close and after dismount/remount on Tevera OS.
- The same volume must remain acceptable to Windows 10 22H2 per the clean-mount acceptance spec.
- Milestone 45 also times read/write performance; absolute MB/s include TCG/IDE. This spec does not set perf numbers—only correctness.

## Probe gates

- New on-disk run-list or attribute-list layouts not already declared in in-tree headers: capture before adding.
- Sparse write allocation remains gated as in [`ntfs-sparse.md`](ntfs-sparse.md).
- Encrypted body remains gated as in [`ntfs-encrypted-file-body.md`](ntfs-encrypted-file-body.md).

## Non-goals

- Copying Microsoft’s cache manager, run map, or I/O scheduling when a different structure in this driver is faster and still matches ReadFile, WriteFile, and on-disk NTFS 3.1 results Windows accepts (Milestone 45 compatibility wording).

## Suite Remount row **(Rev.1)**

**(Rev.1)** Catalog `Remount` in [`tavera/test-suites/ntfs/agent/ntfs_agent.c`](../../test-suites/ntfs/agent/ntfs_agent.c): write bytes, leave the volume as cwd, close the volume handle, optionally `FSCTL_LOCK_VOLUME` ([Learn](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume); fails if any file on the volume is open), then `FSCTL_DISMOUNT_VOLUME` (allowed without a lock per [Learn](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_dismount_volume)), reopen, and read the same bytes. Windows 10 22H2 suite baseline 2026-11-18: green after that harness change (`out/windows-baseline-result.log`).

## Revision log

- **Rev.1** (2026-11-18): Documented suite Remount method; Windows green.
