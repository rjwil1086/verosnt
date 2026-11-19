<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-txf.md
PURPOSE:     Transactional NTFS (TxF) Win32 API acceptance
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS transactional files (TxF API behavior)

## Header and provenance

Component: Transactional NTFS via documented KTM + `CreateFileTransacted` (GitHub #3 transactions slice / **NTFS-TXF-001**).  
Version: 1. Author: Ryan Wilson. Date: 2026-11-18.  
Only allowed sources below were used. Probe session only; this file does not implement `drivers/filesystems/ntfs`.

## Sources

| Source | Date read |
|--------|-----------|
| [How to Use Transactional NTFS](https://learn.microsoft.com/en-us/windows/win32/fileio/how-to-use-transactional-ntfs) | 2026-11-18 |
| [CreateFileTransactedW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfiletransactedw) | 2026-11-18 |
| [CreateTransaction](https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-createtransaction) | 2026-11-18 |
| [CommitTransaction](https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-committransaction) | 2026-11-18 |
| [RollbackTransaction](https://learn.microsoft.com/en-us/windows/win32/api/ktmw32/nf-ktmw32-rollbacktransaction) | 2026-11-18 |
| In-tree `drivers/filesystems/ntfs/rw.c` remarks — transactions not implemented | 2026-11-18 |
| In-tree formatter notes under `sdk/lib/fslib/ntfslib/` — `$Extend` / TxF payload names zeroed on format (names only; no layout invented here) | 2026-11-18 |
| Guest log `output/ntfs/probe-v10/guest-serial.log` from [`tavera/probes/ntfs/probe-v10`](../../probes/ntfs/probe-v10) — marker `NTFS-TXF-PROBE-DONE ok` (Windows 10 22H2) | 2026-11-18 |

## Purpose and scope

In scope: black-box Win32 outcomes for create-transacted → write → commit or rollback, and non-transacted visibility of uncommitted creates.

Out of scope: inventing `$RmMetadata` / `$Txf*` on-disk record layouts; KTM resource-manager internals; DTC. If an implementer needs a named on-disk field not already in this tree, open a raw-format probe item—do not invent it from this API log.

## Interfaces

Documented caller sequence ([Learn](https://learn.microsoft.com/en-us/windows/win32/fileio/how-to-use-transactional-ntfs)):

1. `CreateTransaction` (ktmw32).
2. `CreateFileTransactedW` with that transaction handle.
3. Handle I/O (`WriteFile` / `ReadFile` / close) on the transacted handle.
4. Close transacted handles, then `CommitTransaction` or `RollbackTransaction`.

## Behavior

### Commit

1. Transacted create + write + close of a new file name succeeds.
2. `CommitTransaction` succeeds.
3. Non-transacted `CreateFileW` + `ReadFile` on that path returns the committed bytes.

### Rollback of a new create

1. Transacted create + write + close of a **new** file name succeeds.
2. `RollbackTransaction` succeeds.
3. Non-transacted `CreateFileW` on that path fails as if the file were absent (`ERROR_FILE_NOT_FOUND`).

### Visibility while uncommitted (new create)

1. After transacted create + write (handle still open or closed before commit), a non-transacted open of that new path fails as absent (`ERROR_FILE_NOT_FOUND`) on Windows 10 22H2 per the probe.
2. Rollback after that observation still succeeds.

## Observed behavior

Windows 10 22H2 guest, [`tavera/probes/ntfs/probe-v10`](../../probes/ntfs/probe-v10), `output/ntfs/probe-v10/guest-serial.log`, marker `NTFS-TXF-PROBE-DONE ok`:

| Case | Result |
|------|--------|
| APIs resolved (`ktmw32` + `CreateFileTransactedW`) | ok |
| Format blank secondary disk NTFS | `Format ok` |
| Commit: CreateTransaction / CreateFileTransacted / write / CommitTransaction | all ok |
| Commit: plain read after commit | matches payload |
| Rollback: CreateTransaction / write / RollbackTransaction | ok |
| Rollback: plain open after rollback | fail `e=2` (`ERROR_FILE_NOT_FOUND`) — `absent_expected` |
| Vis: uncommitted write | ok |
| Vis: plain open while uncommitted | fail `e=2` |
| Vis: RollbackTransaction | ok |

## Compatibility notes

- Microsoft documents TxF as available for apps that still call these APIs; Tavera matches the probe outcomes when the product keeps the #3 transactions slice.
- Formatter may already reserve `$Extend` TxF **names**; that is not a substitute for the Win32 commit/rollback behavior above.
- Do not invent stream layouts from the serial log. The log has no hex dumps of metadata.

## Test plan

1. Run [`tavera/probes/ntfs/probe-v10/Invoke-NtfsTxfProbe.ps1`](../../probes/ntfs/probe-v10/Invoke-NtfsTxfProbe.ps1) on Windows 10 22H2; require `NTFS-TXF-PROBE-DONE ok` and the table above.
2. Re-run the same guest binary on Tavera OS amd64; require matching commit visibility, rollback absence (`e=2`), and uncommitted-create invisibility (`e=2`).
3. Host (non-22H2) runs are not evidence.

## Open questions

- Overwrite of an **existing** file under a transaction, and concurrent non-transacted writers — not covered by probe-v10; open a follow-on probe if a guest app needs them.
- On-disk TxF metadata layout — blocked until a raw-format probe is authorized; API acceptance does not require naming private fields.

## Review log

- 2026-11-18: Spec agent — probe-v10 capture + Learn/in-tree only; no driver edit in this session.

## Revision log

- **Rev.0** (2026-11-18): Initial TxF API acceptance from probe-v10.
