<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/test-suites/ntfs/README.md
PURPOSE:     How to run the NTFS 3.1 compliance suite
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# NTFS 3.1 test suite

Milestone 45 / Win10-compatible NTFS feature inventory. Catalog names live in [`features.ps1`](features.ps1) and match [`tavera/specs/ntfs/ntfs-win10-compat.md`](../../specs/ntfs/ntfs-win10-compat.md).

This suite was moved from `buildenv/unittests/ntfs`. QEMU, clang, and path helpers still come from the sibling **buildenv** checkout (`Resolve-VerosPaths.ps1`).

## What this is (and is not)

| Layer | Path | Role |
|-------|------|------|
| **This suite** | `tavera/test-suites/ntfs/` | Green/red feature rows on Tavera and Windows |
| Raw probes | `tavera/probes/ntfs/probe-vN/` | Capture-backed facts for specs (not the suite) |
| ABI diffs | `buildenv/differential-tests/` | Syscall/kernel/NDIS probes |
| ReactOS rostests / kmtests | `modules/rostests/` | **Not** the NTFS 3.1 compliance suite |

**Rule of thumb:** if a catalog row is **red on Windows 10 22H2**, fix this harness before blaming `ntfs.sys`.

## Prerequisites

- Sibling layout: `tavera/source` + `tavera/buildenv` (QEMU under `buildenv/tools/qemu`, clang under `buildenv/tools/clang` or RosBE).
- Built live CD: `source/build-amd64/bootcd.iso`.
- MinGW `x86_64-w64-mingw32-gcc` (RosBE under `buildenv/tools/rosbe/amd64`) to build the freestanding guest agent.
- For Windows baseline: Windows 10 22H2 guest disk under `buildenv/virtual-machines/win10-22h2/`. Default boot is **TCG** (`-m 8192 -smp 4`, visible QEMU window). Optional `-PreferWhpx` tries Hyper-V WHPX first and falls back to TCG (suite troubleshooting only; not a clean-room capture boot).

## Commands

From this directory (or pass full paths):

```powershell
# 1) Windows 10 22H2 baseline only (agent on Windows disk; no Tavera boot)
powershell -File .\Compare-NtfsBytes.ps1 -WindowsOnly

# Optional: faster Windows boot while fixing harness reds
powershell -File .\Compare-NtfsBytes.ps1 -WindowsOnly -PreferWhpx

# 2) Tavera live-CD compliance (patched ISO, agent as verreport.exe)
powershell -File .\Invoke-NtfsSuite.ps1 -Phase compliance

# 3) Windows then Tavera byte-field compare
powershell -File .\Compare-NtfsBytes.ps1

# Other phases: static | stress | fault | fuzz | all
powershell -File .\Invoke-NtfsSuite.ps1 -Phase all
```

## Markers and outputs

- Guest share `result.log`: tab-separated `Name\tgreen|red\t…` rows; ends with `NTFS-AGENT-DONE`.
- Suite snapshot: `out/ntfs31.json` and `out/ntfs31.txt` (green / red / untested, improvements, regressions).
- Runtime under `out/` is gitignored. Do not commit images or agent binaries.

## Catalog ↔ specs (selected)

| Catalog row | Spec |
|-------------|------|
| `Quota` / `QuotaAccounting` / `QuotaDelete` | [`ntfs-quota-on-disk.md`](../../specs/ntfs/ntfs-quota-on-disk.md) — `QuotaDelete` = NtSet `QuotaLimit=-2` + query −1/−1 (probe-v4) |
| `Encryption` / `EfsBody` | [`ntfs-encrypted-file-body.md`](../../specs/ntfs/ntfs-encrypted-file-body.md) — `EfsBody` adds `FILE_ATTRIBUTE_ENCRYPTED`; both need `NTFS-EFS-002` / `NTFS-EFS-004` on Tavera |
| `LogFile` / `LogReplay` / `LogOps` | [`ntfs-log-replay.md`](../../specs/ntfs/ntfs-log-replay.md) — presence vs dirty remount (`LogReplay`) vs create+write+rename+dirty remount (`LogOps`, probe-v7); field layout still `NTFS-LOG-001` |
| `Sparse` | [`ntfs-sparse.md`](../../specs/ntfs/ntfs-sparse.md) |
| `Reparse` / `ReparsePoint` | [`ntfs-reparse.md`](../../specs/ntfs/ntfs-reparse.md) |

Open implementer gaps: [`tavera/specs/ntfs/open-items.md`](../../specs/ntfs/open-items.md).

## Latest scores (2026-11-18)

| Guest | Score | Notes |
|-------|-------|-------|
| Windows 10 22H2 (`-WindowsOnly -PreferWhpx`) | **49 green / 0 red (100%)** | Includes `EfsBody`, `QuotaDelete`, `LogReplay`, `LogOps`. Log: `out/share-windows/result.log` |
| Tavera (`-Phase compliance`) | Catalog **49** rows | `Encryption` / `EfsBody` → **NTFS-EFS-004**; `QuotaDelete` → **NTFS-QUOTA-004**; `LogReplay` / `LogOps` → **NTFS-LOG-001** / **NTFS-LOG-002** |

## Implementer notes

- Prefer `-Phase compliance` while iterating.
- `QuotaDelete` needs `NtSetQuotaInformationFile` with `QuotaLimit=-2` clearing thr/lim to −1 ([`ntfs-quota-on-disk.md`](../../specs/ntfs/ntfs-quota-on-disk.md) Rev.4–5). Do not invent `$Q` layouts.
- `LogReplay` / `LogOps` are black-box dirty remount + readback (`LogOps` under the renamed path); real `$LogFile` parsing stays gated on `NTFS-LOG-001` / `NTFS-LOG-002`.
- `$EFS` framing still `NTFS-EFS-002` (blocks suite `Encryption`).
- Specs: [`tavera/specs/ntfs/`](../../specs/ntfs/) with `**(Rev.N)**` blocks; void [`open-items.md`](../../specs/ntfs/open-items.md).
- Do not edit `drivers/filesystems/ntfs` from a probe/harness session; implement from specs only.
