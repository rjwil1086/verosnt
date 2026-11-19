<!--
PROJECT:     Tevera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/ntfs/ntfs-encrypted-file-body.md
PURPOSE:     Documented encrypted-file API behavior and the missing on-disk body
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS encrypted file body

## Public / in-tree facts

[Handling Encrypted Files and Directories](https://learn.microsoft.com/en-us/windows/win32/fileio/handling-encrypted-files-and-directories) says `EncryptFile` encrypts every data stream in the file, and that a later read through the normal file APIs returns plaintext for a caller who can decrypt it.

[Backup and Restore of Encrypted Files](https://learn.microsoft.com/en-us/windows/win32/fileio/backup-and-restore-of-encrypted-files) says `ReadEncryptedFileRaw` returns the file's data still encrypted.

[EncryptFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-encryptfilew) documents the Win32 encrypt call.

[MS-FSCC: NTFS Attribute Types](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a82e9105-2405-4e37-b2c3-28c773902d85) says `$LOGGED_UTILITY_STREAM` is used by the encrypting file system.

[MS-FSCC: NTFS Stream Names](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6b5e7e75-c2ad-4769-ad96-37077ee69702) names the `$EFS` stream used with encryption metadata.

[MS-EFSR: EFSRPC Raw Data Format](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-efsr/e67520ea-0cb6-4349-bbe5-427a644d9492) says the details of that backup format are implementation-dependent and that a client should not parse it. The page does not say those bytes are the on-disk file body.

[MS-GPEF: Overview](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gpef/4d4687f9-132c-4501-9996-684b40b14735) says a symmetric key encrypts the file data. It does not give the framing of that data on disk.

In-tree:

- Attribute type `AttributeLoggedUtilityStream = 0x100` in `drivers/filesystems/ntfs/ntfs.h` and `sdk/lib/fslib/ntfslib/ntfslib.h`.
- File attribute flag `NTFS_FILE_TYPE_ENCRYPTED` (`0x4000`) in `drivers/filesystems/ntfs/ntfs.h`.
- `NtfsReadEncrypted`, `NtfsWriteEncrypted`, and `NtfsSetEncryption` in `drivers/filesystems/ntfs/ntfs3x.c` return `STATUS_NOT_IMPLEMENTED`. `FSCTL_SET_ENCRYPTION` in `drivers/filesystems/ntfs/fsctl.c` calls `NtfsSetEncryption`.

## Compatibility outcomes

- Callers with decrypt rights who use normal read APIs must see plaintext once encryption is implemented.
- `ReadEncryptedFileRaw` must still be able to obtain ciphertext for backup, per Learn.
- On Windows 10 22H2, `EncryptFileW` on a `DefineDosDevice` drive letter alone returned `ERROR_PATH_NOT_FOUND` in probe-v1; the same file encrypted successfully when the path came from `GetFinalPathNameByHandleW` after create (volume had `FILE_SUPPORTS_ENCRYPTION`).

## Observed behavior (Windows 10 22H2, probe-v1)

Probe: [`tavera/probes/ntfs/probe-v1/`](../../probes/ntfs/probe-v1/). Serial `output/ntfs/probe-v1/guest-serial.log` contains `Encrypt FS_FILE_ENCRYPTION set`, `Encrypt ok finalpath`, and `NTFS-RAW-PROBE-DONE ok`. Manifest `output/ntfs/probe-v1/captures/manifest.txt` ends with `NTFS-RAW-DUMP-END` and `FOUND secret 1`.

Inputs:

- Directory `T:\efs\`, file `T:\efs\secret.txt`.
- 512-byte plaintext pattern `plain[i] = (i * 3 + 1) & 0xff`.
- `GetVolumeInformation` reported `FILE_SUPPORTS_ENCRYPTION`.
- `EncryptFileW(GetFinalPathNameByHandleW(...))` succeeded.

Host dump facts:

| Fact | Capture |
|------|---------|
| MFT record for `secret.txt` (record 39) | `captures/mft_record_39_secret.bin` |
| Attribute types present include `0x10`, `0x30`, `0x80`, and `0x100` | same record (host dump walk) |
| Unnamed `$DATA` non-resident, `lcn=1419`, 512 bytes | `captures/secret_data.bin`; manifest `RUN secret_data` |
| That 512-byte body matches the plaintext pattern in **2/512** bytes only (ciphertext on disk) | compare `secret_data.bin` to the probe pattern |
| `$LOGGED_UTILITY_STREAM` named `$EFS`, non-resident, `lcn=1416`, **672** bytes | `captures/secret_logged_utility.bin`; manifest `BLOB secret_logged_utility` |

### Differential (probe-v2)

Same boot encrypts two 512-byte patterns under `T:\efs\` (`secretA.txt` = `i*3+1`, `secretB.txt` = `i*5+7`) via `EncryptFileW` + final path. Captures under `output/ntfs/probe-v2/A/captures/` (and B):

| Blob | Size | Diff fact |
|------|------|-----------|
| `secretA_data.bin` vs `secretB_data.bin` | 512 / 512 | Almost all bytes differ (ciphertext depends on plaintext / per-file key) |
| `secretA_logged_utility.bin` vs `secretB_logged_utility.bin` | 672 / 672 | Bytes **0..15 identical**; first difference at **offset 16** (16-byte run); further large runs from **offset 416** and **597** (`diff-report.txt` SAMEBOOT-A pair) |
| Cross-boot `secretA_*.bin` A vs B | same sizes | Ciphertext and most of `$EFS` differ again (new FEK per encrypt) |

So the on-disk `$EFS` (`$LOGGED_UTILITY_STREAM`) blob has a **16-byte prefix that is stable for two files encrypted in one session**, then file-specific material. Field names inside that prefix / the 416+ region are **not** assigned without a further input that isolates them.

No encrypted read/write implementation in this probe session.

### Suite results **(Rev.1)**

**(Rev.1)** [`tavera/test-suites/ntfs/`](../../test-suites/ntfs/) agent Encryption row:

| Guest | Result | Detail |
|-------|--------|--------|
| Windows 10 22H2 | **green** | `EncryptFileW` on `GetFinalPathNameByHandleW` under `efs\`; volume had `FILE_SUPPORTS_ENCRYPTION` (`out/windows-baseline-result.log`) |
| Tavera live CD | **red** | `FILE_SUPPORTS_ENCRYPTION clear` (`out/share-compliance/result.log`) |

**(Rev.1)** Tavera must report `FILE_SUPPORTS_ENCRYPTION` on GetVolumeInformation when the volume can support EFS before `EncryptFileW` / `NtfsSetEncryption` work is meaningful. On-disk `$EFS` field names remain `NTFS-EFS-002`.

### Flag vs EncryptFile **(Rev.2)**

**(Rev.2)** Implementer sets `FILE_SUPPORTS_ENCRYPTION` when NTFS major ≥ 3 ([MS-FSA Appendix A](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/4e3695bd-7574-4f24-a223-b4679c065b63)). Open item **NTFS-EFS-003** is **resolved**. Suite detail moved from `FILE_SUPPORTS_ENCRYPTION clear` to `EncryptFile finalpath` (`out/share-compliance/result.log`). `NtfsSetEncryption` remains `STATUS_NOT_IMPLEMENTED` until **NTFS-EFS-002** names `$EFS` fields. There is no stub path that makes the catalog `Encryption` row green without that framing — do not debug `EncryptFileW` in an implementer session.

### Staged `$EFS` recipe (probe-v6) **(Rev.3)**

**(Rev.3)** Probe: [`tavera/probes/ntfs/probe-v6/`](../../probes/ntfs/probe-v6/). Stages **Z** (empty), **S** (512-byte `i*3+1`), **L** (2048-byte), **P** (same-boot secretA/secretB), **R** (encrypt → `DecryptFileW(GetFinalPathNameByHandleW)` → overwrite → encrypt). Serial each stage `NTFS-EFS-PROBE-DONE ok`. Report `output/ntfs/probe-v6/efs-report.txt` (`NTFS-EFS-PROBE-REPORT` … `END`).

| Stage | `$EFS` size | `$DATA` size | Notes |
|-------|-------------|--------------|-------|
| Z | **672** | **0** | empty plaintext |
| S | **672** | **512** | matches probe-v1 size |
| L | **672** | **2048** | `$EFS` size does **not** grow with data |
| P | **672** / **672** | 512 / 512 | A and B both present |
| R | **672** | **512** | re-encrypt after decrypt |

**(Rev.3)** Cross-stage and same-boot facts from `efs-report.txt` + hex of captures:

- Every `$EFS` blob is **exactly 672** bytes (`$LOGGED_UTILITY_STREAM` named `$EFS`).
- Bytes **0..15** are **identical** on every stage and both P files: `A0 02 00 00 00 00 00 00 02 00 00 00 00 00 00 00`. Probe-local labels: `hdr_u32_0` = LE **0x000002A0 (672)**; `hdr_u32_8` = LE **0x00000002**.
- First differing run starts at **offset 16**, length **16**, per file / per encrypt (same-boot P `SAME_PREFIX 16`; also stable header across separate boots).
- Further large differing runs from about **offset 416** (P A vs B: runs at 416, 447, 452, 484) — probe-local `region_416_plus`.
- Unnamed `$DATA` on-disk length equals the plaintext length for these cases (ciphertext body, not plaintext bytes).

**(Rev.3)** **Field-name verdict: none.** No Microsoft FEK / DDF / DRF field names were assigned from Learn or these captures. Probe-local labels (`hdr_u32_0`, `hdr_u32_8`, `region_16_31`, `region_416_plus`) are offsets/sizes only.

**(Rev.3)** **Implementer recipe (structural):** On successful encrypt, create `$LOGGED_UTILITY_STREAM` `$EFS` of length **672** with the cited 16-byte header; keep `$DATA` length equal to the logical data length; set `FILE_ATTRIBUTE_ENCRYPTED` / `NTFS_FILE_TYPE_ENCRYPTED`. Suite rows `Encryption` and `EfsBody` require `EncryptFileW` success + plaintext readback (+ attribute for `EfsBody`). Generating the per-file bytes from offset 16 onward still must not be invented beyond captures — fill those regions only from a cited algorithm or a later probe; until then **NTFS-EFS-004** owns catalog green on Tavera.

### Suite row EfsBody **(Rev.4)**

**(Rev.4)** Catalog `EfsBody` in [`features.ps1`](../../test-suites/ntfs/features.ps1): `EncryptFileW` on final path, require `FILE_ATTRIBUTE_ENCRYPTED`, plaintext readback. Complements `Encryption`. Windows must stay 100% on the expanded catalog before blaming Tavera.

## Non-goals

- Treating MS-EFSR backup blobs as proven on-disk layout without a capture that shows they match.
- Inventing EFS key or FEK framing from recall.
- **(Rev.3)** Assigning Microsoft field names without an allowed source.

## Revision log

- **Rev.1** (2026-11-18): Suite Windows/Tavera Encryption row; Tavera missing `FILE_SUPPORTS_ENCRYPTION`.
- **Rev.2** (2026-11-18): EFS-003 resolved; suite red is EncryptFile / EFS-002.
- **Rev.3** (2026-11-18): probe-v6 Z/S/L/P/R; 672-byte `$EFS`; fixed 16-byte header; field names **none**; structural recipe.
- **Rev.4** (2026-11-18): suite catalog `EfsBody`.
