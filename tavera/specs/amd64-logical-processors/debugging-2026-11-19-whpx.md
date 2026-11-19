<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/amd64-logical-processors/debugging-2026-11-19-whpx.md
PURPOSE:     WHPX live-CD MP desktop gate debugging loop
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# amd64 logical processors — WHPX MP boot debugging

Implementer loop for live CD desktop under `-accel whpx` at `-smp 1`, then `2`, then `4`. Plan: WHPX MP boot. Capture boots stay TCG.

## Harness

- Script: `output/whpx-test-tavera/Boot-WhpxLive.ps1`
- QEMU: `D:\tavera\buildenv\tools\qemu\qemu-system-x86_64.exe`
- Flags: `-accel whpx -cpu qemu64 -m 8192 -cdrom build-amd64/bootcd.iso -boot d -vga qxl -no-reboot -serial file:…` (forward slashes); headless unless `-Gtk`; default `pc` machine (not q35)
- Image: `build-amd64/bootcd.iso`, `DefaultOS=LiveImg_Debug`, `CmdLine=setup -mini`, `ntkrnlmp`/`halmp`
- Success: serial `Parent is Desktop` (not Virt Viewer lifetime)

## Cycles

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| W0 | Fixed harness | `Boot-WhpxLive.ps1` | script under `output/whpx-test-tavera/` | coded |
| W1a | WHPX `-smp 1` `-cpu qemu64` desktop | harness | `sources/debugging-2026-11-19-whpx/boot-whpx-smp1-20261119T131936.log` | **FAIL EXIT:** past Phase1/NDIS/DxStartup; QEMU exits after DxStartup; err `Ignoring request for interrupt vector 0` (earlier chat also saw `failed to decode instruction f 10`) |
| W1b | Same with `-cpu host` | harness `-Cpu host` | `sources/debugging-2026-11-19-whpx/boot-whpx-smp1-host-20261119T132105.log` | **FAIL EXIT:** same class after DxStartup |
| W1c | Windows-like WHPX CPU + q35/qxl | `-cpu qemu64,hv_*` `-machine q35` `-vga qxl` | `sources/debugging-2026-11-19-whpx/boot-whpx-smp1-hv-20261119T132300.log` | **FAIL:** `Fatal System Error 0x7B` (INACCESSIBLE_BOOT_DEVICE) then `kdb:>`; q35 not viable for this live CD |
| W1d | Same hv_* CPU on default `pc` + std VGA | `-cpu qemu64,hv_*` (no q35) | `sources/…/boot-whpx-smp1-hvpc-20261119T132553.log` + `.out` | **FAIL EXIT:** `failed to decode instruction f 10` / `decoder failed` after Bochs VGA / fonts |
| W1e | Avoid Bochs with `-vga none` | `-vga none` | `sources/…/boot-whpx-smp1-vganone-20261119T132716.log` | **FAIL hang:** serial stuck ~93 B for 5+ min |
| W1f | `-vga qxl` on default `pc` | harness now defaults qxl | `sources/…/boot-whpx-smp1-qxlpc-20261119T133234.log` | **PASS Gate A:** `Parent is Desktop` (~60 s); freeldr BiosMode assert still prints |

Evidence copies: `tavera/specs/amd64-logical-processors/sources/debugging-2026-11-19-whpx/`.

## Gate B (smp 2)

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| W2a | Fresh WHPX `-smp 2` baseline with qxl | harness `-Smp 2 -DebugCon` | `sources/…/boot-whpx-smp2-w2a-20261119T133414.log` + debugcon | **HANG:** SIPI `stubPA=5b000`; only one `kiinit` PCR; forever `still waiting CPU 1`; debugcon ASCII `IA` (BSP `I`, AP reaches trampoline `A`, never `B`); err vector 0 |
| W2b | CPU matrix `host` | `-cpu host` qxl smp2 | `sources/…/boot-whpx-smp2-w2b-host-*.log` | **HANG** same; debugcon `IA` |
| W2c | CPU matrix `qemu64,hv_*` | hv flags + qxl | `sources/…/boot-whpx-smp2-w2c-hv-*.log` | **HANG** same; debugcon `IA` |
| W2d | Sub-breadcrumbs A→1→2→3→4→B in `apentry.S` | rebuild | `…/boot-whpx-smp2-w2d-crumbs-*.log` | **HANG** still `IA` — dies on `lgdt` (never `1`) |
| W2e | TCG control same ISO/crumbs | `-accel tcg -smp 2` | `…/boot-smp2-tcg-ctrl-*.log` | **JOIN:** `CPU 1 joined`; debugcon `IA1234BCDEFG?HI` |
| W2f | Real-mode DS==CS; dropped ebp | `apentry.S` | `…/boot-whpx-smp2-w2f-dscs-*.log` | **partial:** debugcon `IA1234BC` then hang; err `WHPX: Unexpected VP exit code 4` (rbp unset for identity loads) |
| W2g | DS==CS **and** keep `ebp=CS<<4` for HalpAPEntry64 | `apentry.S` | `…/boot-whpx-smp2-w2g-ebp-*.log` | **PASS Gate B:** `CPU 1 joined` + `Parent is Desktop`; debugcon `IA1234BCDEFG?HI` |

## Gate C (smp 4)

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| W3a | WHPX `-smp 4` with DS==CS fix | harness `-Smp 4 -DebugCon` | `…/boot-whpx-smp4-w3a-20261119T141143.log` | **PASS Gate C:** `CPU 1/2/3 joined`, `all 4 processors started`, `Parent is Desktop` |

## Done

Gates A–C green. `apentry.S`: real-mode `DS==CS` stub-relative loads + keep `ebp=CS<<4` for identity long-mode pointer loads. Harness defaults `-vga qxl`. `buildenv/.cursor/rules/tavera-test-guest.mdc` updated to allow live-CD WHPX.
