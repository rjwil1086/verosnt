<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/amd64-logical-processors/debugging-2026-11-18.md
PURPOSE:     Record the Milestone 32 amd64 logical-processor implementation debug loop
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# amd64 logical processors — debugging log

Chronological record of the implementer debug loop for Milestone 32, starting when the plan todos were opened. Guest logs stay under gitignored `output/`. This file is the durable trail; chat alone is not enough.

**Harness (unless noted):** QEMU `D:\tavera\buildenv\tools\qemu\qemu-system-x86_64.exe`, `-accel tcg -m 8192`, headless, `-serial file:…`, optional `-debugcon file:… -global isa-debugcon.iobase=0xe9`. Build: `build-amd64\build-halmp-bootcd.cmd` (env.cmd / buildenv). Image: `build-amd64\bootcd.iso` with `KERNEL=NTKRNLMP.EXE HAL=HALMP.DLL` on the debug live path.

**Open items filed first (not debug, but start of the todo work):** ALP-006 (`MultiThreadProcessorSet` vs Ex single-bit cores), ALP-007 (promote exact Ex cache DIFF lines into §6), ALP-008 (class 107 naming). ALP-002 (infinite AP wait) and ALP-005 (>8 APIC) already open; implementer reused i386 infinite wait and deferred >8.

---

## Phase A — Unblocked implementation slices

### A1. Open items
- Appended ALP-006/007/008 to `open-items.md` with implementer context.
- Todo `open-alp-items` completed.

### A2. AP start (first cut)
- Ported i386-shaped loop into `ntoskrnl/ke/amd64/mproc.c` (AP PCR/GDT/IDT/TSS/stacks, `KPROCESSOR_STATE`, `KeLoaderBlock` fields, `HalStartNextProcessor`, wait until `Prcb == 0`).
- Implemented `hal/halx86/smp/amd64/spinup.c` (`ONECPU` / `HalpOnlyBootProcessor`, processor count cap, copy stub, `ApicStartApplicationProcessor`).
- Replaced HLT stub in `hal/halx86/smp/amd64/apentry.S` with real-mode → long-mode trampoline toward `KiSystemStartup`.
- Cap at 64 processors via existing `MAXIMUM_PROC_PER_GROUP` / `KeMaximumProcessors`.

### A3. IPI + TLB
- Implemented `KeIpiGenericCall` in `ntoskrnl/ke/amd64/ipi.c` (Learn KeIpiGenericCall + in-tree i386/`ke/ipi.c` patterns); wired `KiIpiServiceRoutine` packet-ready path.
- Updated `ntoskrnl/ke/amd64/cpu.c` `KeFlushEntireTb` so `AllProcessors` with more than one CPU broadcasts via `KeIpiGenericCall`.

### A4. Ex topology reporting
- Un-stubbed `NtQuerySystemInformationEx` in `dll/ntdll/def/ntdll.spec`.
- Kernel handling for class **107**: cores as single-bit masks, `Flags=0`, package/cache/NUMA/group from started processors and in-tree CPUID types (ALP-006 unresolved → no invented SMT pairing).
- NDK alias consideration noted under ALP-008; numeric wiring uses 107.

### A5. Transparency / interim live options
- Added `tavera/transparency/2026-11-18-amd64-logical-processors.json`.
- Interim `LiveImg_Debug` Options naming `ntkrnlmp`/`halmp`; full winldr live switch still gated on 2-CPU desktop (`live-cd-mp` todo).

### A6. Early build/boot faults (before join)
| Step | Symptom | Change | Result |
|------|---------|--------|--------|
| Build without env | CMake/ninja fail | Use buildenv / `build-halmp-bootcd.cmd` | Builds |
| `lgdt fword` in `.code64` | GAS reject | Non-ML lgdt form | Assembles |
| After VERPORT | APInfo page-align ASSERT | Over-allocate + `ALIGN_UP` | Past that assert |
| AP after final CR3 | Read `LoaderBlock` via identity after map drop | Preload LoaderBlock/ProcessorState before CR3 switch | Still no join |
| Bootcd link | Occasional mkisofs/pe flake | Retry | ISO produced |

Representative early logs (SIPI, no join): `output/boot-m32-20261118T184833Z.log` … `T190940Z.log`.

---

## Phase B — AP trampoline bring-up (isa-debugcon breadcrumbs)

Goal: get past SIPI into `KiSystemStartup` and clear `LoaderBlock->Prcb`.

Breadcrumbs: port `0xE9` letters **A–H** in `apentry.S`, later **I** in `kiinit.c`. Serial: `output/boot-m32-<stamp>.log`. Debugcon: `output/boot-m32-<stamp>.debugcon`.

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| B1 | Need “waiting” before SIPI so a triple-fault still leaves a line | `mproc.c` print waiting before `HalStartNextProcessor` | `T193936Z` and later | Waiting line present; still no join |
| B2 | Die early in trampoline | E9 breadcrumbs A–C | `T193936Z.debugcon` = `ABC` | Dies after C |
| B3 | GAS dropped CS on `PageTableRoot` load → CR3 from phys `0x52` | Load via `[ebp + ZERO_OFFSET(PageTableRoot)]` | Disasm showed bad absolute | CR3 load fixed; still stuck |
| B4 | Still between C and D | Extended breadcrumbs | `T194502Z` still `ABC` | Same |
| B5 | Crash on segment loads under temp GDT | Skip DS/ES/SS loads until real GDT | `T194949Z.debugcon` = `ABCDEFGH` | Reaches H; no join |
| B6 | Crash on jump into `KiSystemStartup` | `retfq` to reload CS | Boot after retfq | Still no join |
| B7 | `retfq` stack under wrong CR3 | Stub-local stack for `retfq` | — | Still no join |
| B8 | Identity RIP dies when switching to BSP CR3 | Jump to `HalpLowStub` VA (`StubKernelVa`) **before** loading BSP CR3 | spinup fills `StubKernelVa` | Progress |
| B9 | Kernel VA execute under temp CR3 fails | Order: HalpLowStub jump → then real CR3 | — | Progress |
| B10 | `retfq` still fails with real GDT | Load SS/DS from real GDT before `retfq` | — | Still fail |
| B11 | Far return fragile | Near `jmp` + 16-byte stack align instead of `retfq` | `T201800Z` | **`CPU 1 joined`** |

Join achieved. `KeStartAllProcessors` then tries CPU 2, HAL reports all processors started (QEMU `-smp 2`), active count 2.

---

## Phase C — Post-join Phase1 assert (`0xC0000420`)

After join, first-chance `STATUS_ASSERTION_FAILURE` in Phase1 during `ObInit` → `ObpCreateDosDevicesDirectory` → `NtCreateDirectoryObject` → `ObCreateObject` → `ObpAllocateObject`.

`ObpAllocateObject` begins with `PAGED_CODE()` (CR8 / `KeGetCurrentIrql() > APC_LEVEL`). Serial often lacks the `NTDDK: Pageable code…` KdPrint line; stack still lands in that function.

| Step | Hypothesis | Change | Log / check | Result |
|------|------------|--------|-------------|--------|
| C1 | First join boot hits assert | Symbolize KDB frames (`ImageBase` `0x400000` + RVA) | `T201800Z` / later | Stack: Phase1 → ObInit → DosDevices → ObpAllocateObject |
| C2 | IRQL left high around Ob/Se | Temporary IRQL/cli hacks in `ex/init.c` / `se/subject.c` | — | Reverted; not the fix |
| C3 | `KiIdleLoop` raises `SYNCH_LEVEL` before `KiSwapContext`; inbound thread resumes elevated | `ke/amd64/thrdini.c` `KiSwapContextResume`: `KeLowerIrql(NewThread->WaitIrql)` when CR8 higher | `T210151Z` | **Still asserts after join** |
| C4 | Scheduler steals Phase1 onto AP | Defer AP bit in `KeActiveProcessors` until after ObInit (`kiinit.c` + `ex/init.c`) | `T151044Z` | **Hang:** AP PCR printed; never clears `Prcb` / no join. Likely AP timer/stall/`KeSetPriorityThread` path needs ActiveProcessors |
| C5 | Revert ActiveProcessors delay; pin Phase1 affinity to BSP (`Affinity=1`) across AP start until after ObInit | `ex/init.c` before `KeStartAllProcessors` / restore after ObInit | `T151603Z`, `T152051Z` | Join works; **assert remains** |
| C6 | Measure CPU/IRQL/affinity at ObInit entry | `DPRINT1` pre-ObInit | `T152051Z`: `Cpu=0 Irql=0 Affinity=0x1 Active=0x3` | Phase1 on BSP at PASSIVE with pin intact; assert still inside ObInit |
| C7 | Confirm MP kernel alone is fine | `-smp 1` same `ntkrnlmp`/`halmp` ISO | `T152407Z-onecpu.log` | ObInit completes (`affinity restored`); boot reaches win32k/services/userland |
| C8 | Assert needs a live AP in `KiIdleLoop` (scheduling/IPI), not merely two MADT CPUs | Park AP after `LoaderBlock->Prcb = 0`, clear its ActiveProcessors bit, `cli` + spin before `KiIdleLoop` | `T152907Z-park.log` | AP prints `AP 1 parked before KiIdleLoop`; **BSP never prints `CPU 1 joined`** (stuck in wait or store visibility / side effect of park path). Diagnostic still open |

**Still open:** why ObInit asserts with a joined, idle-running AP while `-smp 1` and pre-ObInit IRQL 0 on BSP succeed. Candidates not closed: cross-CPU IPI/`KeFlushEntireTb` IRQL, idle SYNCH resume edge not covered by WaitIrql fix, KDB showing BSP stack while AP asserts, or another ASSERT in the Ob path mis-attributed.

---

## Phase D — Verification todos (blocked)

| Todo | Status | Notes |
|------|--------|-------|
| `verify-4cpu` | in progress / blocked | Need 2-CPU past ObInit toward desktop; then ONECPU, probe-v1 on three QEMU shapes, milestone Progress |
| `live-cd-mp` | pending | Only after 2-CPU desktop gate; then syscall-diff |

---

## Boot log index (this loop)

All under `output/` (gitignored). Times are local stamp names.

| Log | Shape | Outcome (short) |
|-----|-------|-----------------|
| `boot-m32-20261118T184833Z` … `T193415Z` | `-smp 2` | SIPI / early die; no join |
| `…T193936Z` (+ `.debugcon`) | `-smp 2` | debugcon `ABC` |
| `…T194949Z` (+ `.debugcon`) | `-smp 2` | debugcon `ABCDEFGH` |
| `…T201800Z` | `-smp 2` | **join** + `0xC0000420` |
| `…T210151Z` | `-smp 2` | join + assert (post WaitIrql fix) |
| `…T151044Z` | `-smp 2` | delayed ActiveProcessors; hang pre-join |
| `…T152051Z` | `-smp 2` | affinity pin; pre-ObInit Irql=0; assert |
| `…T152407Z-onecpu.log` | `-smp 1` | ObInit OK; userland |
| `…T152907Z-park.log` | `-smp 2` | AP parked; BSP no `joined` line |

---

## Files touched in this loop (implementation + debug)

- `tavera/specs/amd64-logical-processors/open-items.md`
- `ntoskrnl/ke/amd64/mproc.c`, `kiinit.c`, `krnlinit.c`, `ipi.c`, `cpu.c`, `thrdini.c`
- `hal/halx86/smp/amd64/apentry.S`, `spinup.c`
- `ntoskrnl/ex/init.c`, `ntoskrnl/ex/sysinfo.c` (Ex class 107)
- `dll/ntdll/def/ntdll.spec` (+ related NDK/Wine wiring as landed)
- `tavera/transparency/2026-11-18-amd64-logical-processors.json`
- Interim boot CD live options for MP kernel/HAL

---

## Phase E — Park diagnostic continued (2026-11-18 afternoon)

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| E1 | BSP missed `Prcb = 0` | Interlocked clear + yield then park | `…T153645Z-park2.log` | **Join + pre-ObInit** (`Irql=0 Affinity=0x1 Active=0x3`); then AP parked (`Active=0x1`). **ObInit hangs** (no assert). |
| E2 | Hang = IPI wait on cli-parked AP still in Active at send | Clear Active before yield | `…T154138Z-park3.log` | Hang after AP PCR; never reached park (AP frequency stall / TCG). |
| E3 | Skip AP `KiCalculateCpuFrequency` | Copy BSP MHz; breadcrumbs | `…T154721Z-park4.log` | AP reaches “cleared Prcb; yielding”; **BSP never joins**; yield-with-IF=1 never finishes park print. |
| E4 | Interrupt-enabled yield starves BSP | Immediate cli + YieldProcessor park | `…T155110Z-park5.log` | Stuck after `KeSetPriorityThread` print (before Prcb clear) — **SetPriority can schedule away before clear**. |
| E5 | Clear Prcb before SetPriority | Park before SetPriority | `…T155507Z-park6.log` | Stuck after skip-frequency (DISPATCH raise?). |
| E6 | Park from APC_LEVEL, no IRQL raise | Immediate park at APC | `…T155854Z-park7.log` | Stuck after “parking from APC_LEVEL” (DPRINT/lock?). |
| E7 | Avoid KD lock; E9 PQR | `__outbyte` P/Q/R; plain/`InterlockedExchange64` on `KeLoaderBlock->Prcb` | `…T160532Z-park9` / `…T160959Z-park10` debugcon `…MNPQR` | **AP reaches R (Prcb cleared, Active dropped, parked). BSP never prints joined** (no “still waiting” either → BSP barely scheduled). |
| E8 | YieldProcessor starves; use `__halt` | cli + `__halt` loop | `…T161327Z-park11.debugcon` `…PQR` | Same: AP parked, **BSP still no join**. |

### Interim conclusions from Phase E

1. E1 showed: with join + AP removed from useful participation, ObInit **hangs** rather than asserting — likely **IPI/TLB wait** on a non-responsive AP.
2. Later park variants often clear Prcb (E9 `Q`) but BSP does not observe join under TCG when AP is in a tight park — scheduling/visibility artifact; park is a weak diagnostic under this harness.
3. `KeSetPriorityThread` on AP before Prcb clear can prevent clear (E4).
4. AP `KiCalculateCpuFrequency` / stall is unsafe before Prcb clear under TCG (E2/E3).

### E9 (next) — drop park; disable TLB IPI; normal idle

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| E9 | ObInit assert/hang tied to `KeIpiGenericCall` shootdown | Restore AP → `KiIdleLoop`; WaitIrql fix + Phase1 affinity pin + AP MHz copy; **`KeFlushEntireTb` local-only** (`&& FALSE`) | `…T161804Z-e9.log` | **PASS:** join; pre-ObInit; **affinity restored after ObInit**; SMSS; **win32k**; winlogon/desktop activity; NDIS/Ext2. No `0xC0000420`. |

### Root cause (current)

`KeFlushEntireTb` → `KeIpiGenericCall` breaks Phase1 ObInit on `-smp 2` (`PAGED_CODE` / CR8 > APC in `ObpAllocateObject`). With that call disabled (E9), 2-CPU boot reaches win32k/winlogon. One-phase `KeIpiGenericCall` rewrite (E10) still asserts at the same place — shootdown stays local-only until IPI is fixed for real.

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| E10 | One-phase DoneCount + `KiIpiPacketRequest[]` fixes shootdown | Rewrite `ke/amd64/ipi.c`; re-enable IPI in `KeFlushEntireTb` | `…T162453Z-ipi2.log` | join + pre-ObInit; **`0xC0000420` again** (ObpAllocateObject PAGED_CODE). |
| E11 | Nested SYNCH around `KeIpiGenericCall` was the IRQL bug | `KeFlushEntireTb` lets `KeIpiGenericCall` own SYNCH | `…T163338Z-ipi3.log` | Still **`0xC0000420`** at same ObInit point. Shootdown stays **local-only** (E9 behavior) while IPI is redesigned. |

## Boot log index (additions)

| Log | Shape | Outcome (short) |
|-----|-------|-----------------|
| `…T162453Z-ipi2.log` | one-phase IPI | assert after pre-ObInit |
| `…T163338Z-ipi3.log` | IPI owns SYNCH | assert after pre-ObInit |
| `…T165251Z-e12.log` | breadcrumbs + shootdown | IPI IRQL OK; assert after first IPI |
| `…T170128Z-e13.log` | noop IPI | still assert (HalRequestIpi enough) |
| `…T170609Z-e14.log` | Affinity=0 no IPI | ObInit/win32k OK |
| `…T171039Z-e15.log` | PreviousIrql fix + shootdown | **ObInit OK; win32k; IPI Done=1** |

## Boot log index (additions)

| Log | Shape | Outcome (short) |
|-----|-------|-----------------|
| `…T153645Z-park2.log` | park | join + pre-ObInit; ObInit hang |
| `…T154138Z-park3.log` | park | hang after AP PCR |
| `…T154721Z-park4.log` | park | AP clear+yield; no BSP join |
| `…T155110Z-park5.log` | park | stuck at SetPriority |
| `…T155507Z`–`T161327Z-park11` | park + E9 | PQR on debugcon; BSP no join |
| `…T161804Z-e9.log` | idle, no TLB IPI | **ObInit OK; win32k/desktop path** |

### E12 — identify which CPU hits PAGED_CODE

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| E12 | Assert CPU may be AP while KDB shows BSP; or caller leaves CR8 high | Re-enable `KeFlushEntireTb`→`KeIpiGenericCall`; DPRINT1 CPU/IRQL in `KeIpiGenericCall`, packet handler, and `ObpAllocateObject` when Irql>APC | `…T165251Z-e12.log` | **IPI path IRQL OK:** `IPI call Cpu=0 OldIrql=0 Synch=12`; `IPI packet Cpu=1 Irql=14`; `IPI done WantIrql=0 HaveIrql=0 Done=1`. **No** `ObpAllocateObject HIGH`. Then `0xC0000420`. KDB frames (ImageBase `0x400000`): `KeReleaseQueuedSpinLock` → `MiResolveDemandZeroFault` → `MiDispatchFault` → `MmArmAccessFault` → page-fault return. Prior Ob/`PAGED_CODE` attribution was wrong for this failure. |

### E12 conclusions

1. `KeIpiGenericCall` raises/lowers SYNCH correctly on the BSP; AP runs the packet at IPI_LEVEL.
2. Failure is immediately after the first shootdown, in the MM demand-zero / PFN queued-lock path — not `ObpAllocateObject`.
3. No RtlAssert `*** Assertion failed` text on serial (fits `NT_ASSERT` / `DbgRaiseAssertionFailure` / `PAGED_CODE` style, or a truncated KD report).

### E13 — noop IPI vs TB flush on AP

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| E13 | Any IPI broadcast during ObInit breaks MM; or only AP `KeFlushCurrentTb` | `KeFlushEntireTb` multi path: `KeIpiGenericCall` with noop worker, then local `KeFlushCurrentTb` only | `…T170128Z-e13.log` | **Still `0xC0000420`** after first noop IPI (`IPI done HaveIrql=0`). Stack: `MiRemoveAnyPage` → `MiResolveDemandZeroFault` → `MiDispatchFault` → `MmArmAccessFault`. **HalRequestIpi / AP interrupt path is enough**; AP TB flush not required. |

### E14 — KeIpiGenericCall without HalRequestIpi

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| E14 | SYNCH + `KiReverseStallIpiLock` alone breaks MM; or only `HalRequestIpi` | Force `Affinity=0` in `KeIpiGenericCall` (no IPI send); local TB worker | `…T170609Z-e14.log` | **PASS:** `IPI E14: suppressing Affinity=0x2`; affinity restored; win32k; winlogon. No assert. |

### E15 — KiIpiInterrupt PreviousIrql before ExitTrap

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| E15 | `ExitTrap` DBG `ASSERT_TRAP_FRAME_IRQL_VALID` (`trapamd64.inc`): CR8 still IPI_LEVEL vs saved PreviousIrql → `int 0x2c` / `0xC0000420` | Restore PreviousIrql before `ExitTrap` in `KiIpiInterrupt` (same as `KiInterruptDispatch`); re-enable real shootdown; remove E14 Affinity=0 | `…T171039Z-e15.log` | **PASS:** join; IPI packet Cpu=1 Irql=14; IPI done Done=1; affinity restored; **assert_hits=0**; further IPI shootdowns continue. |

### Root cause (resolved)

`KiIpiInterrupt` raised CR8 to IPI_LEVEL then called `ExitTrap`, whose DBG `ASSERT_TRAP_FRAME_IRQL_VALID` requires CR8 == `PreviousIrql` (`sdk/include/asm/trapamd64.inc`). Mismatch → `int 0x2c` / `0xC0000420`. Fix: restore PreviousIrql before ExitTrap (in-tree pattern from `KiInterruptDispatch` in the same file).

## Verification (post-E15)

| Step | Check | Evidence | Result |
|------|-------|----------|--------|
| V1 | `-smp 2` + shootdown → win32k/winlogon | `…T171039Z-e15.log` | PASS |
| V2 | `ONECPU` starts only BSP (`-smp 2` QEMU) | `…T171847Z-onecpu.log` | PASS (`ONECPU`; active count 1; win32k) |
| V3 | `-smp 4` joins 4 CPUs; ObInit/win32k | `…T172209Z-smp4.log` | Join+ObInit+win32k PASS; later DBG `Thread->State == Running` (`ke_x.h:1344`) at winlogon TLS — separate from IPI ExitTrap |
| V4 | LiveImg / LiveImg_Debug → `ntkrnlmp`/`halmp`; `LiveImg_ONECPU` entry | `boot/bootdata/bootcd.ini` | done |
| V5 | probe-v1 Ex shapes on Tavera | ALP-006/007 still open for SMT/cache bit-exact; Ex single-bit cores match §6 | deferred to open items |

### F1 — smp4 `Thread->State == Running` (V3 follow-up)

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| F1 | Need actual `State` value at `KxQueueReadyThread` | DbgPrint State/Cpu/Thr/Aff/Idle before ASSERT | `…T174346Z-f1.log` | Assert **not** reproduced; User AV in `mpr.dll` then bugcheck `0x1e`/`C0000005` during winlogon |
| F2 | `KiDpcInterruptHandler` queues Idle onto ready lists while `KiIdleLoop` never does; IdleLoop also claimed `NextThread` without `PrcbLock` so DPC could race | Skip Idle in `KxQueueReadyThread` callers (DPC/ExitDispatcher/QuantumEnd); IdleLoop claims `NextThread` under lock; remove F1 DbgPrint | `…T175405Z-f2.log` | **PASS:** 4 CPUs joined; affinity restored; past late TLS (`msgina`/`shsvcs`); setup `-mini` starts; no `Thread->State == Running` assert |

### G1 — ALP-005 Tavera `-smp 9`

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| G1 | MP kernel/HAL + shootdown past 8 logical processors | Boot existing ISO `-smp 9` (no code change) | `…T175600Z-smp9.log` | **PASS:** CPUs 1–8 joined; affinity `0x1ff` after ObInit; win32k loads; no IPI/TLB hang. ALP-005 resolved with this Tavera note |

Update this file in the same turn as each new hypothesis → change → log → result cycle.

### H1 — probe-v1 Tavera shapes blocked before RunSetup

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| H1 | Live Setup `CmdLine=topology_probe.exe` + COM1 free (LiveImg) or COM2 KD would reach DIFF | Rebuilt bootcd with probe in `reactos.cab`, hive CmdLine, LiveImg then LiveImg_Debug+COM2 dual serial | `output/amd64-logical-processors/probe-v1/tavera-4c1t-boot2.log` (Affinity=`0xf` after ObInit; `DxStartupDxgkInt`; no `Started setup`); earlier remasters `Fatal 0x1e` in LSASS/`apphelp` with `probeLen=0` | **Blocked:** guest reaches win32k/winlogon Dx path then hangs or bugchecks before `topology_probe` prints DIFF. F2 (`…T175405Z-f2.log`) still the last boot that started `setup -mini`. Temporary livecd CmdLine/DefaultOS/cmake probe hooks reverted. |

### H2 — live CD after setup: mouse freeze / soft hang on MP

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| H2a | Rebuild empty ISO; stock `setup -mini` | `build-halmp-bootcd.cmd` | `bootcd.iso` ~908MB; freeldr LiveImg_Debug + ntkrnlmp/halmp | ISO OK |
| H2b | Baseline `-smp 4` topology | headless LiveImg_Debug | `…T203949Z-baseline.log` | **PASS** markers: join, affinity `0xf`, `Started setup`, `Parent is Desktop` |
| H2c | Interactive desktop confirm | QEMU window `-smp 4` topology | `…T204358Z-desktop.log` | Blue screen then **`Fatal 0x1e`**: WDF null / later runs **User AV LSASS** execute at `lsasrv!__wine_dbch_lsasrv` (`.data`) with RBP `__dyn_tls_init` |
| H2d | SMP bisect | `-smp 1`, `-smp 2`, `-smp 4` | `…T204735Z-smp1` desktop OK; `…T205058Z-smp2` desktop OK; `…T205102Z-smp4r` early LSASS `0x1e` | Failures concentrate on 4-CPU; also flaky |
| H2e | Plain `-smp 4` like F2 | headless | `…T210015Z-smp4plain`, `…T210330Z-smp4topo2` | Desktop markers, no Fatal while watched |
| H2f | Interactive plain `-smp 4` | window `Tavera-M32-Desktop` | `…T210750Z-desktop2.log` | Past installing devices; **mouse froze**; serial **stopped growing**; QEMU CPU high; last lines NDIS5 OID loop — soft hang (not bugcheck) |
| H2g | Hang MP-specific? | `-smp 1` hangcheck | `…T211202Z-smp1-hangcheck.log` | Log **keeps growing** (LLDP TX / NDIS SEND) — not hung |
| H2h | `-smp 2` hangcheck | headless | `…T211554Z-smp2-hangcheck.log` | Soft hang early (~30KB, pre-setup); CPU high |
| H2i | `KeIpiGenericCall` infinite DoneCount wait | Re-arm packet + re-`HalRequestIpi` on stall; give up + DPRINT after long stall | `ntoskrnl/ke/amd64/ipi.c` | Retest `-smp 4` |
| H2j | Re-arm still hangs | Keep re-arm; remove give-up | `…T213212Z-ipirearm.log` | **FAIL:** Parent is Desktop then serial freeze mid-umpnpmgr; QEMU CPU high. Filed [#46](https://github.com/taveraos/taveraos/issues/46). Temporary local-only TB flush (`cpu.c` `&& FALSE`) for e9local PASS. |
| H2k | Silent LiveImg + RunOnce probe | LiveImg (no `/DEBUG`), RunOnce `topology_probe.exe tavera` | `output/.../probe-v1/tavera-4c1t.log` | **FAIL:** 17 min, serial stuck at freeldr heap lines only (2017 B); no `DIFF`. LiveImg without KD is silent after freeldr; RunOnce never observed. |
| H2l | LiveImg_Debug menu sendkey | telnet monitor down×4 + ret | `…T040400Z-livedebug-sanity.log` | **FAIL:** stayed at freeldr (sendkey unreliable headless). |
| H3 | Broadcast IPI is the hang; probe delivery needs Userinit | Sequential single-CPU `HalRequestIpi` + DoneCount wait; restore AllProcessors shootdown; `DefaultOS=LiveImg_Debug`; RunOnce expand-SZ + Userinit; probe COM1→COM2 | `…T041300Z-h3-seqipi.log` | **FAIL:** join+ObInit+win32k OK; serial froze at 23199 B during winlogon/services (pre-setup); QEMU CPU pegged; **no** `KeIpiGenericCall: stall` DPRINT — wait may not be the spin, or DPRINT never flushed |
| H4 | DoneCount wait vs IPI delivery | Fire-and-forget: arm flags, single-CPU `HalRequestIpi`, **no** DoneCount spin; still run worker on caller | `…T043000Z-h4-nowait.log` | **Partial:** reached `Started setup` + `Parent is Desktop` + umpnpmgr; then soft hang at NDIS5 OID loop (same as H2f). **Conclusion:** hang is IPI_LEVEL flush delivery under load, not DoneCount wait alone. |
| H5 | Flush at DISPATCH via DPC | Remove `ASSERT(Prcb==CurrentPrcb)` in `KeInsertQueueDpc`; `KeFlushEntireTb` queues HighImportance targeted DPCs (wake via `IPI_DPC`); keep `KeIpiGenericCall` for the API | `…T044000Z-h5-dpctb.log` | **PASS hangcheck:** join+ObInit+`Started setup`+`Parent is Desktop`; serial grew 6KB→625KB+ over ~15 min with freeze=0; NDIS OID loop keeps moving; QEMU CPU ~140 not pegged. IPI_LEVEL `__writecr3` was the soft-hang; DPC shootdown stays live. |

### H6 — probe starts but no DIFF on dual serial

| Step | Hypothesis | Change | Evidence | Result |
|------|------------|--------|----------|--------|
| H6a | COM2-first + dual `-serial` captures DIFF under LiveImg_Debug | Boot `-serial kd -serial probe` | `tavera-4c1t.kd.log` / `.log` | **FAIL capture:** `WL: Started setup 'topology_probe.exe'`; KD shows `WARNING: calling stub GetLogicalProcessorInformationEx` then `ExitProcess(0)`; probe COM2 file stuck at freeldr HEAP lines (2017 B); **no `DIFF` on either serial** |
| H6b | kernel32 Ex export is still `-stub`; COM CreateFile fails under KD so emit is silent | Implement `GetLogicalProcessorInformationEx` → `NtQuerySystemInformationEx` in `kernel32/client/sysinfo.c`; drop `-stub` in `kernel32.spec`; ungate `NtQuerySystemInformationEx` (sysfuncs.h + ntdll.spec + exfuncs.h + sysinfo.c NTDDI); probe `emit_raw` also `OutputDebugStringA` | rebuild ntdll+ntkrnlmp+kernel32+probe+bootcd | **PASS 4c1t:** `tavera-4c1t.log` has `DIFF END OK`, `DIFF LOGICAL 4`, four single-bit cores `flags=0`. No `DIFF REL cache` lines (CPUID leaf 4 null on this QEMU cpu). |
| H6c | qemu64-2c2t first boot | same ISO | `tavera-qemu64-2c2t.kd.log` | **FAIL once:** after `Started setup`, `RtlpWaitForCriticalSection` Deadlock → KDB INT3; no DIFF. |
| H6d | Retry qemu64-2c2t (flake) | same ISO, kill QEMU before extract | `tavera-qemu64-2c2t.log` | **PASS:** `DIFF END OK`, `DIFF LOGICAL 4`, four single-bit cores; CPUID SMT `shift=1`. |
| H6e | Skip COM CreateFile (hang risk) | `open_com` returns `INVALID_HANDLE_VALUE`; OutputDebugString only | probe source updated | done (source); captures used prior COM-skip ISO with OutputDebugString |
| H6f | epyc-2c2t | dual serial LiveImg_Debug | `tavera-epyc-2c2t.log` | **PASS:** `DIFF END OK`, `DIFF LOGICAL 4`, four single-bit cores |
| H6g | Restore `KeIpiGenericCall` DoneCount barrier | `KiIpiGenericCallNoWait = FALSE`; TB stays on H5 DPC path | rebuild ntkrnlmp+bootcd; setup `-mini` restored | pending hangcheck after syscall-diff |

