<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-pagefile.md
PURPOSE:     Paging-file create and ordinary-open acceptance for NTFS
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS paging file (API behavior)

## Header and provenance

Component: paging-file creation on an NTFS volume and ordinary Win32 opens of that path (GitHub #3 page-file slice / **NTFS-PAGE-001**).  
Version: 1. Author: Ryan Wilson. Date: 2026-11-18.  
Only allowed sources below were used. Probe session only; this file does not implement `drivers/filesystems/ntfs`.

## Sources

| Source | Date read |
|--------|-----------|
| [IRP_MJ_CREATE (FS and FS Filters)](https://learn.microsoft.com/en-us/previous-versions/windows/drivers/ifs/irp-mj-create) — `SL_OPEN_PAGING_FILE` means the create is for a paging file | 2026-11-18 |
| [Win32_PageFileSetting](https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-pagefilesetting) — `SeCreatePagefilePrivilege` | 2026-11-18 |
| [IFS Test](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/wlk/ff563405(v=vs.85)) — CreatePagingFileTest: ordinary open of a paging file should fail | 2026-11-18 |
| In-tree `sdk/include/ndk/mmfuncs.h` — `NtCreatePagingFile` | 2026-11-18 |
| In-tree `ntoskrnl/mm/pagefile.c` — privilege check; `MINIMUM_PAGEFILE_SIZE` (= 256×`PAGE_SIZE`) | 2026-11-18 |
| In-tree `drivers/filesystems/ntfs/create.c` — commented `SL_OPEN_PAGING_FILE` / `PagingFileCreate` | 2026-11-18 |
| In-tree `drivers/filesystems/ntfs/rw.c` remarks — page files not implemented on write path | 2026-11-18 |
| Guest log `output/ntfs/probe-v9/guest-serial.log` from [`tavera/probes/ntfs/probe-v9`](../../probes/ntfs/probe-v9) — marker `NTFS-PAGE-PROBE-DONE ok` (Windows 10 22H2) | 2026-11-18 |

## Purpose and scope

In scope: black-box acceptance so a Tavera NTFS volume can host a paging file created the same way Windows 10 22H2 accepts (`NtCreatePagingFile` with privilege), and so ordinary `CreateFileW` on that path fails as Windows does.

Out of scope: inventing pager I/O internals; claiming a Win32 way to set `SL_OPEN_PAGING_FILE`; WMI next-boot pagefile settings; on-disk layouts beyond what ordinary create/write already require.

## Interfaces

### Create path (caller)

- Entry already in this tree: `NtCreatePagingFile` (`mmfuncs.h` / ntdll).
- Caller must hold **SeCreatePagefilePrivilege** ([Win32_PageFileSetting](https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-pagefilesetting); in-tree `pagefile.c` checks it).
- Arguments used by the probe (probe sizes, not Windows fields): NT path `\??\<letter>:\pagefile.sys`, minimum 2 MiB, maximum 4 MiB (both above in-tree `MINIMUM_PAGEFILE_SIZE`).
- File-system create that backs a paging file is associated with `SL_OPEN_PAGING_FILE` on the create stack ([Learn](https://learn.microsoft.com/en-us/previous-versions/windows/drivers/ifs/irp-mj-create)). User-mode Win32 does not set that IRP flag directly.

### Ordinary open (non-paging)

- After a successful paging-file create, ordinary `CreateFileW` on the same DOS path must **not** succeed for normal data access.
- Probe also tried `SYNCHRONIZE`-only desired access with full share flags (IFS Test style); that open must also fail on Windows 10 22H2.

## Behavior

1. With privilege enabled, `NtCreatePagingFile` on a fresh NTFS volume path succeeds (`STATUS_SUCCESS`).
2. The volume must accept the create (allocation / file object path suitable for a paging file). Implementer enables the in-tree `SL_OPEN_PAGING_FILE` create recognition and the paging-file I/O path called out in `rw.c` remarks—without inventing undocumented on-disk structures.
3. Subsequent ordinary `CreateFileW` (read/write or synchronize-only) on that path fails with a Win32 sharing/access failure (see Observed).
4. Do **not** require a separate in-driver export named for chkdsk; unrelated to **NTFS-CHKDSK-001**.

## Observed behavior

Windows 10 22H2 guest, [`tavera/probes/ntfs/probe-v9`](../../probes/ntfs/probe-v9), `output/ntfs/probe-v9/guest-serial.log`, marker `NTFS-PAGE-PROBE-DONE ok`:

| Step | Result |
|------|--------|
| Format blank secondary disk NTFS | `Format ok` |
| Enable `SeCreatePagefilePrivilege` | ok |
| Resolve `NtCreatePagingFile` | ok |
| `NtCreatePagingFile` (2 MiB / 4 MiB) | `0x00000000` |
| `CreateFileW` GENERIC_READ\|WRITE after create | fail `e=32` (`ERROR_SHARING_VIOLATION`) |
| `CreateFileW` SYNCHRONIZE-only after create | fail `e=32` (`ERROR_SHARING_VIOLATION`) |

## Compatibility notes

- Host WMI pagefile **settings** are not a substitute for live `NtCreatePagingFile` on the volume under test.
- Tavera must match the success + denied-open outcomes above on the same probe recipe when the suite or a differential guest runs it.
- Closing this slice does not close TxF (**NTFS-TXF-001**).

## Test plan

1. Run [`tavera/probes/ntfs/probe-v9/Invoke-NtfsPagefileProbe.ps1`](../../probes/ntfs/probe-v9/Invoke-NtfsPagefileProbe.ps1) on Windows 10 22H2; require `NTFS-PAGE-PROBE-DONE ok` and the status/`e=` lines above.
2. Re-run the same guest binary (or suite row) on Tavera OS amd64; require the same `NtCreatePagingFile` success and ordinary `CreateFileW` failures (`e=32` or equivalent mapped status).
3. Do not treat a host (non-22H2) run as evidence.

## Open questions

- Exact non-cached / Mm paging I/O semantics beyond create+deny-open — only implement what the probe and Learn require; if a later guest log names a new failure, extend this spec with a Rev mark.

## Review log

- 2026-11-18: Spec agent — probe-v9 capture + Learn/in-tree only; no driver edit in this session.

## Revision log

- **Rev.0** (2026-11-18): Initial pagefile API acceptance from probe-v9.
