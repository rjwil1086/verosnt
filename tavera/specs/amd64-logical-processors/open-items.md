<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/amd64-logical-processors/open-items.md
PURPOSE:     Spec and implementer handoff queue for amd64 logical processors
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# amd64 logical processors open items

Handoff file for implementer ↔ spec agent. Do not delete entries. See `.cursor/rules/tavera-specs-open-items.mdc`.

## Format

```text
### ID
- Status: open | deferred | resolved
- Opened: YYYY-MM-DD
- Question: …
- Blocks: …
- Resolved: YYYY-MM-DD (when resolved)
- Resolution: cite source or probe; link to spec change
- Deferred: YYYY-MM-DD (when deferred)
- Deferral: why no implementable spec yet; what input unblocks it
```

## Items

### ALP-001
- Status: resolved
- Opened: 2026-11-18
- Question: On Windows 10 22H2 build 19045, what exact `GetLogicalProcessorInformationEx(RelationAll)` record sequence, core masks, `Flags`, package, cache, NUMA, and group fields appear for (a) `-smp 4,sockets=1,cores=4,threads=1`, (b) `-cpu qemu64 -smp 4,sockets=1,cores=2,threads=2`, and (c) `-cpu EPYC -smp 4,sockets=1,cores=2,threads=2`?
- Blocks: Filling section 6 observed behavior with capture-backed numbers; implementer comparison of Ex output beyond the milestone's high-level shapes.
- Resolved: 2026-11-18
- Resolution: Captured by `tavera/probes/amd64-logical-processors/probe-v1`. Logs `tavera/specs/amd64-logical-processors/sources/probes/probe-v1/windows-4c1t.log`, `windows-qemu64-2c2t.log`, `windows-epyc-2c2t.log` each contain `DIFF END OK`. All three shapes reported `DIFF LOGICAL 4`, `DIFF CORE_COUNT 4`, four cores with `flags=0` and single-bit masks. Spec section 6 updated. The milestone's "2 cores with pair masks / LTP_PC_SMT" expectation for `threads=2` did not appear on Windows under these QEMU guests.

### ALP-002
- Status: resolved
- Opened: 2026-11-18
- Question: What timeout does this tree use when waiting for an application processor to enter `KiSystemStartup`, and is any public document required, or is an in-tree constant enough?
- Blocks: Completing the AP start task without inventing a duration from memory.
- Resolved: 2026-11-18
- Resolution: Accept the in-tree i386 wait in `ntoskrnl/ke/i386/mproc.c` (spin until `KeLoaderBlock->Prcb == 0`; no finite timeout). Spec sections 4 and 5 updated. No Learn duration; do not invent milliseconds.

### ALP-003
- Status: resolved
- Opened: 2026-11-18
- Question: Does this tree already export `NtQuerySystemInformationEx` for `SystemLogicalProcessorInformationEx`, or must the information class be added under `ntoskrnl` for kernelbase's existing `GetLogicalProcessorInformationEx` wrapper?
- Blocks: Wiring the Ex report path; does not block filling `MultiThreadProcessorSet` or AP start.
- Resolved: 2026-11-18
- Resolution: In-tree only. `dll/ntdll/def/ntdll.spec` declares `NtQuerySystemInformationEx` with `-stub -version=0x601+`. `dll/win32/kernelbase/wine/memory.c` already calls it for `GetLogicalProcessorInformationEx`. Implementer must un-stub the export and handle `SystemLogicalProcessorInformationEx` so the wrapper returns the section 6 shape. No guest probe required.

### ALP-004
- Status: resolved
- Opened: 2026-11-18
- Question: For the three QEMU success shapes, does CPUID report die or module levels so `RelationProcessorDie` / `RelationProcessorModule` records must appear?
- Blocks: Exact Ex die/module presence; does not block core/package/NUMA/group success criteria.
- Resolved: 2026-11-18
- Resolution: Same probe-v1 logs. `DIFF EX_DIE_RECORDS 0`, `DIFF EX_MODULE_RECORDS 0`, `DIFF CPUID_DIE 0`, `DIFF CPUID_MODULE 0` on all three shapes. Spec section 6 updated.

### ALP-005
- Status: resolved
- Opened: 2026-11-18
- Question: On a guest with more than eight logical processors, what capture shows IPI delivery by physical APIC ID (and x2APIC when needed)?
- Blocks: Closing the "past 8 processors" IPI task with evidence; four-CPU success criteria can land first.
- Note: API-behavior probes cannot observe APIC delivery without forbidden debugger or memory inspection. Implementation may verify Tavera `-smp >8` does not hang on TLB/IPI; that is not Windows APIC evidence.
- Resolved: 2026-11-18
- Resolution: Tavera evidence only (not Windows APIC capture). `tavera/specs/amd64-logical-processors/sources/probes/boot-m32-20261118T175600Z-smp9.log`: QEMU `-smp 9`, MP kernel/HAL; CPUs 1–8 joined; Phase1 affinity restored to `0x1ff` after ObInit; `win32k.sys` loads. No TLB/`KeIpiGenericCall` hang past eight logical processors. Physical APIC ID / x2APIC delivery on Windows remains unobservable under clean-room probe rules.

### ALP-006
- Status: resolved
- Opened: 2026-11-18
- Question: On QEMU `threads=2`, CPUID leaf `0xB` reports SMT (`level_type=1`, `shift=1`) while Ex cores are four single-bit masks with `flags=0` (source 27 / section 6). Should `KPRCB.MultiThreadProcessorSet` (a) stay single-bit so it matches Ex and existing `SystemLogicalProcessorInformation`, or (b) hold CPUID SMT sibling masks so `KeStartThread`'s `~MultiThreadProcessorSet` preference can place two runnable threads on different cores, with Ex filled separately to match source 27?
- Blocks: Milestone placement success ("prefer two cores over two siblings"); whether topology fill may disagree with Ex core records.
- Resolved: 2026-11-18
- Resolution: Choice (a). `MultiThreadProcessorSet` equals the Ex core mask (single-bit / `flags=0` on source 27 shapes). Do not invent CPUID SMT pairs in Ex or in that field. Spec sections 3–5 and 9 updated. Placement uses existing `~MultiThreadProcessorSet` against Ex core sets.

### ALP-007
- Status: resolved
- Opened: 2026-11-18
- Question: Section 6 says "per-core L1/L2 and one L3 covering `0xf`" but does not paste exact `DIFF REL cache` fields (Level, Associativity, LineSize, CacheSize, Type, mask). Please promote those lines from `tavera/specs/amd64-logical-processors/sources/probes/probe-v1/windows-4c1t.log` / `windows-qemu64-2c2t.log` / `windows-epyc-2c2t.log` into section 6 so the implementer does not have to re-interpret the capture beyond what the spec states.
- Blocks: Bit-exact Ex cache comparison on Tavera differential runs.
- Resolved: 2026-11-18
- Resolution: Exact `DIFF REL cache` lines from each probe-v1 Windows log are now in spec section 6.

### ALP-008
- Status: resolved
- Opened: 2026-11-18
- Question: Wine/`kernelbase` use `SystemLogicalProcessorInformationEx = 107`; NDK `sdk/include/ndk/extypes.h` names 107 `SystemLogicalProcessorAndGroupInformation`. Confirm implementer should treat class 107 as the Ex topology class (ALP-003) and whether NDK should gain an alias name for clarity.
- Blocks: Naming only; numeric wiring can proceed as 107.
- Resolved: 2026-11-18
- Resolution: Class 107 is the Ex topology class. `sdk/include/ndk/extypes.h` already aliases both `SystemLogicalProcessorAndGroupInformation` and `SystemLogicalProcessorInformationEx` to 107. No further NDK rename. Spec source 28 and section 4 updated.

### ALP-002 note from implementer (2026-11-18)
- While ALP-002 stayed open, amd64 AP start reused the in-tree i386 wait in `ntoskrnl/ke/i386/mproc.c` (spin until `KeLoaderBlock->Prcb == 0`, no finite timeout). Spec accepted that wait (ALP-002 resolved).

### ALP-009
- Status: resolved
- Opened: 2026-11-19
- Resolved: 2026-11-19
- Question: On the same QEMU shapes, Windows emits section 6 `DIFF REL cache` lines while Tavera emits none (source 29). Probe-v1 never dumped CPUID leaf `0x4` (`CPUID_CACHE_PARAMS`) or `0x8000001D` (`CPUID_CACHE_PARAMS_AMD`). What subleaf values does Windows see on those guests, and must Ex cache records be filled from those leaves via the in-tree decode types?
- Blocks: Bit-exact Ex cache match on Tavera; closing the cache gap without inventing sizes.
- Resolution: Probe-v2 (spec source 33; `tavera/specs/amd64-logical-processors/sources/probes/probe-v2/windows-*.log`, each `DIFF END OK`) on the same three shapes: leaf `0x4` is `cache4 sub=0 type=null` on every shape. On `4c1t` and `qemu64-2c2t`, `amdcache` is absent (`max=0x8000000a`) while Ex still emits the section 6 cache lines. On `epyc-2c2t`, `amdcache` subleaves 0–3 report sizes 32768 / 65536 / 524288 / 8388608 that match that shape’s Ex lines; subleaf 4 is null; leaf `0x4` stays null. Spec sections 4–6 and 9 updated: Ex cache records must match section 6 even when both leaves are empty or absent; leaf `0x8000001D` is optional corroboration when present, not a required sole fill path.

### Implementer note E12–E15 (2026-11-18) — not a new open item
- E12/E13: first `HalRequestIpi` during ObInit → `0xC0000420`; IPI IRQL on BSP was fine; MM stacks were a red herring for KD frame listing.
- E14: `KeIpiGenericCall` with Affinity forced to 0 (no IPI) → ObInit/win32k/winlogon OK (`…T170609Z-e14.log`).
- E15 fix (in-tree): `KiIpiInterrupt` raised CR8 to IPI_LEVEL then `ExitTrap` ran `ASSERT_TRAP_FRAME_IRQL_VALID` (`sdk/include/asm/trapamd64.inc`) which requires CR8 == PreviousIrql → `int 0x2c`. Restore PreviousIrql before ExitTrap (same as `KiInterruptDispatch` in `trap.S`). See `debugging-2026-11-18.md`.
- V2 ONECPU OK (`…T171847Z-onecpu.log`). V3 `-smp 4` joins 4 CPUs and passes ObInit/win32k; later DBG assert `Thread->State == Running` at `ke_x.h:1344` during winlogon TLS (`…T172209Z-smp4.log`) — follow-up, not the IPI ExitTrap bug. Live CD Options now select `ntkrnlmp`/`halmp`.

### ALP-010
- Status: resolved
- Opened: 2026-11-19
- Resolved: 2026-11-19
- Question: `Test-WindowsProbe.ps1 -Name syscall-diff` reports `CAPTURE` / probe-changed (stamped probe=c34e417f... vs current differential-tests/syscall-diff/probe.c identity). Re-capture `output/syscall-diff/windows.log` with `DIFF END OK` on Windows 10 22H2 and refresh `windows.probe`.
- Blocks: Milestone 8 paired compare that may reuse the Windows stamp; implementer continues Tavera-only capture without a Windows boot.
- Resolution: Re-captured Windows 10 22H2 on QEMU TCG (`output/syscall-diff/windows.log`, `DIFF END OK`). Stamped `output/syscall-diff/windows.probe` (`probe=448b542b…`, `log=be557c44…`). `Test-WindowsProbe.ps1 -Name syscall-diff` now prints `REUSE syscall-diff`. Implementer can pair Tavera without another Windows boot until `probe.c` changes again.
- Note (2026-11-19): Tavera `output/syscall-diff/reactos.log` already has `DIFF END OK` from a temporary Setup CmdLine of the syscall probe. A FAT `A:\probe.exe` RunOnce under `setup -mini` reached desktop then `kdb:>`; RunOnce was removed from `livecd.inf`.

## Queue check

2026-11-19 (implementer): Every revision assigned to the implementer is implemented. `open-items.md` has no **open** items left (ALP-001…010 resolved). Stopped the 10-minute wake.

2026-11-19 (spec): After Ryan’s message restart rule: every item is already **resolved** (none **open** or **deferred**). No idle 10-minute poll armed.

2026-11-19 (implementer, after Ryan message restart): Re-read `open-items.md`. Every item is still **resolved** (ALP-001…010); none **open**. No implementer revisions pending. No idle 10-minute poll armed. Continuing unblocked WHPX Gate A harness work.
