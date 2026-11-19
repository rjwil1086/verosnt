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

## Observed behavior (Windows 10 22H2, probe-v1)

Probe: [`tavera/probes/ntfs/probe-v1/`](../../probes/ntfs/probe-v1/). Guest serial `output/ntfs/probe-v1/guest-serial.log` contains `Format ok`, `LogTraffic ok`, and `NTFS-RAW-PROBE-DONE ok`. Host dump `output/ntfs/probe-v1/captures/manifest.txt` contains `NTFS-RAW-DUMP` … `NTFS-RAW-DUMP-END`.

After FormatEx + create/rename/delete traffic on the blank volume:

| Fact | Capture |
|------|---------|
| Volume starts at image offset 1048576; boot OEM `NTFS    ` | `captures/boot_sector.bin` |
| `bps=512`, `spc=8`, `cluster=4096`, `mft_lcn=5376`, `rec_bytes=1024` | same manifest `FIELD` lines |
| `$LogFile` is MFT record 2 | `captures/mft_record_2_logfile.bin` (1024 bytes) |
| `$LogFile` data run `lcn=4862` `clusters=512` | manifest `RUN logfile_data` |
| First 1048576 bytes of that run | `captures/logfile_data.bin` |

### Differential (probe-v2)

| Variant | Log traffic | Capture |
|---------|-------------|---------|
| A | create only (`logtraffic.bin` left on volume) | `output/ntfs/probe-v2/A/captures/logfile_data.bin` |
| B | create + rename + delete | `output/ntfs/probe-v2/B/captures/logfile_data.bin` |

Both dumps are 1048576 bytes. `output/ntfs/probe-v2/diff-report.txt` reports **27939** differing runs between A and B. Early pages also differ at many **sector-tail** offsets (every 512 bytes in the first 16 sectors), consistent with update-sequence / fixup style bookkeeping already used elsewhere in NTFS multi-sector structures in this tree — not enough by itself to name restart-page or log-record fields.

Restart-page / log-record field names and offsets inside `logfile_data.bin` remain **unassigned**. Until those fields are cited from an allowed source, `NtfsReplayLog` stays a no-op success and log record writing stays empty. This probe session must **not** edit `NtfsReplayLog` / `NtfsLogFileRecord`.

### Staged recovery (probe-v5) **(Rev.1)**

**(Rev.1)** Probe: [`tavera/probes/ntfs/probe-v5/`](../../probes/ntfs/probe-v5/). Stages **C** (format), **W** (write+flush known 512-byte file), **D** (W + `fsutil dirty set` + dismount/remount + readback), **X** (W + create/rename/delete). Serial each stage `NTFS-LOG-PROBE-DONE ok`. Report `output/ntfs/probe-v5/log-report.txt` (`NTFS-LOG-PROBE-REPORT` … `END`).

| Stage | `$LogFile` dump size | 0xFF byte count (first 1 MiB) | Serial markers |
|-------|----------------------|-------------------------------|----------------|
| C | 1048576 | 887884 | format only |
| W | 1048576 | 867387 | write known ok |
| D | 1048576 | 900089 | dirty set ok; remount NTFS ok; **readback ok** |
| X | 1048576 | 855391 | extra rename-delete ok |

**(Rev.1)** Captures under `output/ntfs/probe-v5/<stage>/captures/logfile_data.bin`. Differentials still have thousands of runs (format→write **9242**, write→dirty-remount **10907**, write→extra **10239**). Early pages again show 512-byte-spaced single-byte changes (fixup cadence). **No restart-page or log-record field names assigned.**

**(Rev.1)** Black-box fact for a later suite `LogReplay` row: on Windows 10 22H2, after writing known bytes, `fsutil dirty set`, volume dismount/remount, the volume remounts as NTFS and the known file bytes read back equal ([fsutil dirty](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-dirty)). That check does **not** require naming `$LogFile` fields. Real `NtfsReplayLog` parsing remains blocked on **NTFS-LOG-001**.

### Suite row LogReplay **(Rev.2)**

**(Rev.2)** Catalog row `LogReplay` is in [`tavera/test-suites/ntfs/features.ps1`](../../test-suites/ntfs/features.ps1). Agent path ([`ntfs_agent.c`](../../test-suites/ntfs/agent/ntfs_agent.c)): write known bytes to `logreplay.bin`, `fsutil dirty set`, `FSCTL_DISMOUNT_VOLUME` / remount, assert NTFS + readback equal. Same black-box contract as probe-v5 stage D; still does **not** name `$LogFile` fields (**NTFS-LOG-001** open).

### Single-op isolation (probe-v7) **(Rev.3)**

**(Rev.3)** Probe: [`tavera/probes/ntfs/probe-v7/`](../../probes/ntfs/probe-v7/). Stages **F** (format), **C** (empty create `logiso.bin`), **W** (write 512 known bytes), **N** (rename to `logiso2.bin`), **X** (delete), **T** (W + `fsutil dirty set` + dismount/remount + readback). Each stage serial ends `NTFS-LOG-ISO-DONE ok`. Report `output/ntfs/probe-v7/log-iso-report.txt` (`NTFS-LOG-ISO-REPORT` … `NTFS-LOG-ISO-REPORT-END`).

| Stage | `$LogFile` dump size | 0xFF byte count (first 1 MiB) | Serial markers |
|-------|----------------------|-------------------------------|----------------|
| F | 1048576 | 884294 | format only |
| C | 1048576 | 879188 | create empty ok |
| W | 1048576 | 863970 | write known ok |
| N | 1048576 | 871660 | rename ok |
| X | 1048576 | 867715 | delete ok |
| T | 1048576 | 883526 | dirty set ok; remount NTFS ok; **readback ok** |

**(Rev.3)** Pairwise dump diffs (`DIFFRUNS` / `FIXUP_CADENCE_HITS` from the report):

| Pair | DIFFRUNS | FIXUP_CADENCE_HITS (first 64 runs) |
|------|----------|-------------------------------------|
| format→create | 7968 | 16 |
| create→write | 7865 | 16 |
| write→rename | 7998 | 16 |
| rename→delete | 9180 | 16 |
| write→dirty-remount | 7908 | 16 |

**(Rev.3)** Structural recipe (probe-local labels; not Microsoft field names): every stage keeps a **1048576**-byte first-run dump; each single metadata op still moves **thousands** of byte runs; early pages again show **512-byte-spaced** single-byte changes (`FIXUP_CADENCE_HITS` = 16 in the sampled prefix of every pair), consistent with update-sequence / fixup cadence already used elsewhere in this tree. 0xFF fill shrinks on create/write and partly recovers after dirty remount. Captures: `output/ntfs/probe-v7/<stage>/captures/logfile_data.bin`.

**(Rev.3)** **Field-name verdict: none.** Restart-page and log-record Microsoft names remain unassigned. Remaining uncited bytes close via **Windows↔Tavera differentials** after an implementer applies this recipe (same ops; compare `$LogFile` dumps and guest-visible status) — open item **NTFS-LOG-002**. This probe session does **not** edit `NtfsReplayLog` / `NtfsLogFileRecord`.

**(Rev.3)** Black-box fact for suite `LogOps`: on Windows 10 22H2, create empty file → write known bytes → rename → `fsutil dirty set` → dismount/remount → remount as NTFS and readback equal under the **new** name (probe stages C/W/N/T guest-visible slice). Still does not name `$LogFile` fields.

### Suite row LogOps **(Rev.4)**

**(Rev.4)** Catalog row `LogOps` is in [`tavera/test-suites/ntfs/features.ps1`](../../test-suites/ntfs/features.ps1). Agent path ([`ntfs_agent.c`](../../test-suites/ntfs/agent/ntfs_agent.c)): create/write `logops.bin`, rename to `logops2.bin`, `fsutil dirty set`, dismount/remount, assert NTFS + readback equal under `logops2.bin`. Complements `LogReplay` (original name). Cite probe-v7 C/W/N/T black-box; **NTFS-LOG-001** field layout still open.

### Dirty-mount restart recognition (probe-v11 / GitHub #2) **(Rev.5)**

**(Rev.5)** Implementer recipe for recognizing restart-tagged pages and empty client slots **without** applying records: [`ntfs-log-restart-mount.md`](ntfs-log-restart-mount.md). Analysis: [`tavera/probes/ntfs/probe-v11`](../../probes/ntfs/probe-v11) / `output/ntfs/probe-v11/restart-report.txt` (`NTFS-LOG-RESTART-REPORT` … `END`) over probe-v7 dumps. **NTFS-LOG-003** resolved. Microsoft field names remain **none** (**NTFS-LOG-001**).

## Non-goals

- Inventing restart-page or log-record layouts from model recall.
- Copying Microsoft’s internal log scheduling into Tevera OS.

## Revision log

- **Rev.1** (2026-11-18): probe-v5 C/W/D/X; dirty remount readback ok; field layout still open.
- **Rev.2** (2026-11-18): suite catalog `LogReplay` wired to dirty remount + readback.
- **Rev.3** (2026-11-18): probe-v7 F/C/W/N/X/T single-op recipe; field names none; LOG-002 for Win↔Tavera differentials.
- **Rev.4** (2026-11-18): suite catalog `LogOps` (rename + dirty remount readback).
- **Rev.5** (2026-11-18): probe-v11 restart-page dirty-mount recipe for GitHub #2 (`ntfs-log-restart-mount.md`).
