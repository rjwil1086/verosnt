<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-log-replay.md
PURPOSE:     Documented NTFS log recovery behavior and the missing record layout
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS log replay

## Public / in-tree facts

[MS-FSCC: NTFS Reserved File Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b04c3bd0-79dc-4e58-b8ed-74f19fc2ea0a) lists `$LogFile` as the transactional logging file.

[How NTFS Works](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc781134(v=ws.10)) says that during recovery NTFS redoes each committed transaction found in the log, then rolls back transactions that were not committed. The same page says the log records for a transaction are written to disk before the metadata that transaction changes.

[NTFS overview](https://learn.microsoft.com/en-us/windows-server/storage/file-server/ntfs-overview) says NTFS uses its transaction log to restore file system consistency after a system failure.

[Fsutil dirty](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty) documents querying and setting the volume dirty bit; when set, autochk checks the volume at the next restart.

In-tree:

- `sdk/lib/fslib/ntfslib/files.c` fills `$LogFile` clusters with `0xFF` at format and comments that a driver treats an all-`0xFF` log as uninitialized.
- `drivers/filesystems/ntfs/ntfs.h` defines `NTFS_VOLUME_DIRTY` (`0x0001`) on `$VOLUME_INFORMATION` and `VCB_VOLUME_READ_ONLY` when a dirty `$LogFile` has not been replayed (reads proceed; writes do not).
- `NtfsReplayLog` in `drivers/filesystems/ntfs/ntfs3x.c` returns `STATUS_SUCCESS` and ignores the volume. Mount calls it from `drivers/filesystems/ntfs/fsctl.c` when `VCB_VOLUME_READ_ONLY` is set. `NtfsLogFileRecord` in the same file is empty.

None of the Learn pages above, and no declaration in `drivers/filesystems/ntfs/ntfs.h`, give the byte layout of a restart page or a log record.

## Compatibility outcomes

- After a clean format with an uninitialized (`0xFF`) log, Windows 10 22H2 and Tevera OS must mount the volume without requiring a full chkdsk repair solely because of the empty log (see also [`ntfs-windows-clean-mount.md`](ntfs-windows-clean-mount.md)).
- When the volume dirty bit is set, documented recovery is redo of committed transactions then undo of uncommitted ones. Exact record parsing is **not** specified here yet.

## Probe gate (raw-format)

A future **probe session** must:

1. Cause Windows 10 22H2 to write real `$LogFile` content via [`tavera/probes/ntfs/probe-v1/`](../../probes/ntfs/probe-v1/) (documented volume activity only; no debugger).
2. Store sector or stream captures under `output/ntfs/probe-v1/`.
3. Extend **this** spec with every new size, offset, magic, or field name cited to a capture path and probe step.

That session must **not** edit `NtfsReplayLog` / `NtfsLogFileRecord`. A later **implementation session** that has not interpreted the raw captures—only this updated spec—may implement replay.

Until then, `NtfsReplayLog` stays a no-op success and log record writing stays empty.

## Non-goals

- Inventing restart-page or log-record layouts from model recall.
- Copying Microsoft’s internal log scheduling into Tevera OS.
