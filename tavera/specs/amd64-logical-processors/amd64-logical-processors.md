<!--
PROJECT:     Tavera OS
LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
FILE:        tavera/specs/amd64-logical-processors/amd64-logical-processors.md
PURPOSE:     Specify amd64 logical processors, SMT topology, IPI, and reporting for milestone 32
COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>
-->

# amd64 logical processors

## 1. Header and provenance

- **Component:** amd64 logical processors
- **Version:** not assigned. There is no product-version bump.
- **Author:** Ryan Wilson <ryan@taveraos.com>
- **Date:** 2026-11-18
- **Provenance:** Only allowed sources were used. They are the public documents and in-tree files in section 2. Recalled Windows internals are not a source. A missing citation stops the edit.

This file is the spec the implementation follows. A later code change that disagrees with the spec updates the spec in the same change.

This spec is an implementation spec for milestone 32. It is not a raw-format probe spec. It does not interpret guest disk sectors, hive bytes, or other persisted Windows structures. Host signing tools stay staplers. They do not supply an offset, a prototype, or a layout.

Work that changes behavior under this spec follows `.cursor/rules/clean-room.mdc`, `.cursor/rules/clean-room-third-party-sources.mdc`, `.cursor/rules/clean-room-symbols-and-debuggers.mdc`, `.cursor/rules/clean-room-serial-capture.mdc`, `.cursor/rules/clean-room-probe-separation.mdc`, `.cursor/rules/clean-room-model-recall.mdc`, `.cursor/rules/clean-room-exposure.mdc`, and `.cursor/rules/clean-room-host-tools.mdc`. A forbidden input that is opened or pasted stops the component. The session logs that exposure in `contamination-log.md` outside `source/` and does not continue the component.

## 2. Sources

Every claim below names one of these entries (Learn URL, in-tree path, or guest log). Dates read are 2026-11-18 unless noted.

1. [GetLogicalProcessorInformationEx function (sysinfoapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformationex). Relationship types, buffer sizing, `ERROR_INSUFFICIENT_BUFFER`, and `RelationAll`.
2. [SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX (winnt.h)](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-system_logical_processor_information_ex). Record layout: `Relationship`, `Size`, and the union members for processor, NUMA, cache, and group.
3. [LOGICAL_PROCESSOR_RELATIONSHIP (winnt.h)](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ne-winnt-logical_processor_relationship). `RelationProcessorCore`, `RelationNumaNode`, `RelationCache`, `RelationProcessorPackage`, `RelationGroup`, `RelationProcessorDie`, `RelationProcessorModule`, `RelationAll`.
4. [PROCESSOR_RELATIONSHIP (winnt.h)](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-processor_relationship). For a core, `Flags` is `LTP_PC_SMT` when the core has more than one logical processor, else 0. `GroupCount` is 1 for a core. `EfficiencyClass` is nonzero only on heterogeneous cores; this milestone leaves hybrid classes out, so cores report 0.
5. [KeIpiGenericCall function (wdm.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keipigenericcall). Broadcast worker on every processor, barrier, return value from the calling processor, IRQL below `IPI_LEVEL`.
6. [KeQueryActiveProcessors function (ntddk.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-kequeryactiveprocessors). Affinity mask of active processors; on Windows 7 and later the mask is group 0.
7. `sdk/include/xdk/ketypes.h` in this tree. `LTP_PC_SMT` is `0x1`. `MAXIMUM_PROC_PER_GROUP` is 64. `LOGICAL_PROCESSOR_RELATIONSHIP`, `PROCESSOR_RELATIONSHIP`, `SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX`, `CACHE_RELATIONSHIP`, `NUMA_NODE_RELATIONSHIP`, `GROUP_RELATIONSHIP`.
8. `sdk/include/ndk/amd64/ketypes.h` in this tree. `KPRCB.MultiThreadProcessorSet` and `MultiThreadSetMaster`.
9. `sdk/include/reactos/x86x64/Intel/Cpuid.h` in this tree. `CPUID_EXTENDED_TOPOLOGY` (`0x0B`), `CPUID_V2_EXTENDED_TOPOLOGY` (`0x1F`), SMT level type `0x01`, core level type `0x02`, module `0x03`, die `0x05`, and x2APIC feature bit on leaf `0x01`. Leaf `CPUID_CACHE_PARAMS` (`0x04`) for deterministic cache parameters (ALP-009).
10. `sdk/include/reactos/x86x64/Amd/Cpuid.h` in this tree. `CPUID_VIR_PHY_ADDRESS_SIZE` (`0x80000008`) with `NC` and `ApicIdCoreIdSize`, and `CPUID_AMD_PROCESSOR_TOPOLOGY` (`0x8000001E`) with `CoreId` and `ThreadsPerCore`.
11. `ntoskrnl/ke/amd64/mproc.c` in this tree. amd64 site for `KeStartAllProcessors`; required behavior is in sections 4 and 5.
12. `ntoskrnl/ke/i386/mproc.c` in this tree. Existing AP start loop: allocate per-processor structures, call `HalStartNextProcessor`, wait for the processor to join, stop on failure or limit.
13. `hal/halx86/smp/amd64/spinup.c` in this tree. amd64 site for `HalStartNextProcessor`; required behavior is in sections 4 and 5.
14. `hal/halx86/smp/i386/spinup.c` in this tree. `ONECPU` / `HalpOnlyBootProcessor` returns `FALSE`; otherwise starts the next AP through the low stub and APIC start path already in that file.
15. `hal/halx86/generic/halinit.c` in this tree. Load option `ONECPU` sets `HalpOnlyBootProcessor`.
16. `hal/halx86/apic/apicsmp.c` and `hal/halx86/acpi/madt.c` in this tree. IPIs and INIT/SIPI use `LapicId`. MADT notes an 8-bit local APIC ID and that x2APIC is not wired yet.
17. `ntoskrnl/ke/amd64/ipi.c` in this tree. amd64 site for `KeIpiGenericCall`; required behavior is Learn source 5 and sections 4 and 5.
18. `ntoskrnl/ke/amd64/cpu.c` in this tree. Site for `KeFlushEntireTb`; when `AllProcessors` is set and more than one processor is active, every active processor must flush (sections 4 and 5).
19. `ntoskrnl/ke/thrdobj.c` in this tree. `KeStartThread` prefers a mask of `~MultiThreadProcessorSet` intersected with the process affinity and node mask when that intersection is non-empty.
20. `ntoskrnl/ex/sysinfo.c` in this tree. `SystemLogicalProcessorInformation` fills core records from `MultiThreadSetMaster` / `MultiThreadProcessorSet`. Class 107 is the Ex path (source 28).
21. `dll/win32/kernelbase/wine/memory.c` in this tree. `GetLogicalProcessorInformationEx` calls `NtQuerySystemInformationEx` with `SystemLogicalProcessorInformationEx`.
22. `boot/bootdata/txtsetup.sif` in this tree. Entries for `ntkrnlmp.exe` and `halmp.dll` already exist beside the uniprocessor images.
23. `boot/freeldr/freeldr/ntldr/winldr.c` in this tree. Default kernel and HAL names are `ntoskrnl.exe` and `hal.dll` until selection overrides them.
24. `ntoskrnl/ke/amd64/kiinit.c` in this tree. `KiSystemStartup` is the kernel entry. Boot PRCB sets `MultiThreadProcessorSet` to `SetMember` today. `StartCycles` is initialized from the timestamp counter.
25. `ntoskrnl/ke/thrdobj.c` `KeQueryTotalCycleTimeThread` in this tree. A running thread's cycle total uses the current processor cycle time and that processor's `StartCycles`.
26. GitHub milestone [Milestone 32: amd64 logical processors](https://github.com/taveraos/taveraos/milestone/31) (API number 31). Project goal, task list, success criteria, one group of at most 64 logical processors, and scope exclusions.
27. Guest serial logs from `tavera/probes/amd64-logical-processors/probe-v1` under `output/amd64-logical-processors/probe-v1/`, each containing `DIFF END OK`: `windows-4c1t.log`, `windows-qemu64-2c2t.log`, `windows-epyc-2c2t.log`. Windows 10 22H2 on QEMU TCG, `-m 8192`, `-snapshot`. Date captured: 2026-11-18.
28. `sdk/include/ndk/extypes.h` in this tree. Class `107` is named both `SystemLogicalProcessorAndGroupInformation` and `SystemLogicalProcessorInformationEx` (same value).
29. Tavera OS guest serial logs from the same probe under `output/amd64-logical-processors/probe-v1/`, each containing `DIFF END OK`: `tavera-4c1t.log`, `tavera-qemu64-2c2t.log`, `tavera-epyc-2c2t.log`. Date captured: 2026-11-19.
30. Tavera OS boot log `output/boot-m32-20261118T175600Z-smp9.log` (`-smp 9`): CPUs 1–8 joined, Phase1 affinity `0x1ff`, no TLB/IPI hang. Tavera-only evidence; not Windows APIC delivery.
31. [PROCESSOR_GROUP_INFO (winnt.h)](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-processor_group_info). `MaximumProcessorCount` and `ActiveProcessorCount`.
32. `sdk/include/reactos/x86x64/Cpuid.h` in this tree. `CPUID_CACHE_PARAMS_AMD` (`0x8000001D`) for AMD deterministic cache parameters.
33. Guest serial logs from `tavera/probes/amd64-logical-processors/probe-v2` under `output/amd64-logical-processors/probe-v2/`, each containing `DIFF END OK`: `windows-4c1t.log`, `windows-qemu64-2c2t.log`, `windows-epyc-2c2t.log`. Same Windows 10 22H2 QEMU TCG shapes as source 27. Date captured: 2026-11-19. Adds `DIFF CPUID cache4` (leaf `0x4`) and `DIFF CPUID amdcache` (leaf `0x8000001D`) beside the Ex cache lines.

No WDK or SDK headers, symbols, disassembly, or third-party struct dumps are sources. Geoff Chappell, Vergilius, syscall tables built from Microsoft binaries, and undocumented-API sites are not sources. Learn debugger command pages are not sources.

## 3. Purpose and scope

GitHub milestone API number 31 is [Milestone 32: amd64 logical processors](https://github.com/taveraos/taveraos/milestone/31). The project name is Tavera OS. This spec does not rename existing C file headers.

The component starts every logical processor on the amd64 boot for a machine that fits in one processor group, fills `MultiThreadProcessorSet` so Ex core records match the Windows captures in source 27, prefers another Ex core via the existing `~MultiThreadProcessorSet` path when two threads can run, delivers TLB shootdown and generic IPI broadcasts across those processors, and reports the topology through `GetLogicalProcessorInformationEx`.

In scope:

- Application-processor start through `KeStartAllProcessors` and `HalStartNextProcessor`, with `ONECPU` keeping only the boot processor.
- `KeIpiGenericCall`, TLB shootdown that reaches every started processor, and IPI delivery by physical APIC ID. Use x2APIC when the physical APIC ID does not fit in the 8-bit xAPIC width (sources 9, 16). A Tavera `-smp 9` boot with LapicIds that still fit in eight bits (source 30) does not by itself prove x2APIC.
- Running-thread cycle time taken from the processor the thread is on (source 25).
- Intel topology leaves `0x1F` or `0x0B` and AMD leaves `0x80000008` and `0x8000001E` for die/module presence (and diagnostics). Ex core masks and `MultiThreadProcessorSet` follow source 27, not invented CPUID sibling pairs. Ex cache records match source 27 / section 6; leaf `0x4` / `0x8000001D` observations are source 33 (ALP-009).
- User-visible topology from `GetLogicalProcessorInformationEx` (information class 107, source 28).
- Live CD selection of `ntkrnlmp.exe` and `halmp.dll` after a 2-CPU desktop boot succeeds. Uniprocessor images stay on the CD.

Left out on purpose (source 26):

- Processor groups beyond group 0, multi-node NUMA, hybrid efficiency classes, and core parking.
- Changing `KTHREAD` or `KPRCB` layouts.
- Machines with more than 64 logical processors keep the first 64 only (`MAXIMUM_PROC_PER_GROUP` in source 7).
- Milestone 34 work does not land in the middle of this work.
- Physical memory above the ceilings already specified for milestone 31.

The session that writes this spec does not implement the component. Implementation is a later session that has not run the Windows topology probe for this milestone.

Each edited C or assembly file gains the Ryan Wilson copyright line when implementation lands. The comment under that header lists the sources that edit used. The implementation commit includes a dated `tavera/transparency/` note. Update the milestone description in the same turn as implementation work: check off a finished task bullet, and replace only the `## Progress` section. Commit when asked. Do not push `upstream`.

## 4. Interfaces

Names and types already in this tree. Required behavior is stated here; calling conventions stay as declared.

- **`KeStartAllProcessors`** (sources 11, 12, 26). Starts each remaining application processor up to the active limit. After `HalStartNextProcessor` succeeds, wait until the AP clears `KeLoaderBlock->Prcb` the same way source 12 does (spin with `KeMemoryBarrier` / `YieldProcessor`). No finite timeout is specified. On success the active processor set grows. `ONECPU` (source 15) causes no application processor to start.
- **`HalStartNextProcessor`** (sources 13, 14). Parameters stay `PLOADER_PARAMETER_BLOCK` and `PKPROCESSOR_STATE`. Returns `TRUE` when the next AP was launched, `FALSE` when `ONECPU` is set, when no further AP remains, or when start fails.
- **`KiSystemStartup`** (source 24). Entry for the boot processor and for each application processor after it reaches long mode (source 26).
- **`KeIpiGenericCall`** (sources 5, 17). Parameters `PKIPI_BROADCAST_WORKER BroadcastFunction` and `ULONG_PTR Context`. Runs the worker on every active processor after a barrier. Returns the worker's return value from the calling processor. Callers run below `IPI_LEVEL`.
- **`KeFlushEntireTb`** (source 18). When `AllProcessors` is set and more than one processor is active, every active processor flushes its TLB.
- **IPI delivery** (sources 16, 26, 30). Delivery uses the target's physical APIC ID. When that ID does not fit in the 8-bit xAPIC width, use x2APIC (sources 9 and 16). Source 30 shows Tavera past eight logical processors without a hang; it does not show Windows APIC/x2APIC wiring.
- **`KPRCB.MultiThreadProcessorSet` / `MultiThreadSetMaster`** (sources 8, 20, 24, 27). Each logical processor's `MultiThreadProcessorSet` is the affinity mask of its Ex `RelationProcessorCore` record (including itself). On the source 27 QEMU shapes that mask is a single bit (`SetMember`). `MultiThreadSetMaster` is that PRCB. Do not widen the set to CPUID SMT siblings when Ex reports single-bit cores.
- **`KeStartThread` affinity preference** (source 19). When the complement of the seed's `MultiThreadProcessorSet` still intersects the allowed mask, the new thread's ideal processor is chosen from that complement first. Evaluated against Ex core sets, not invented CPUID pairs.
- **`GetLogicalProcessorInformationEx`** (sources 1, 2, 3, 4, 7, 21, 28). User-mode entry already in kernelbase. It must succeed once the kernel answers information class `107` (`SystemLogicalProcessorInformationEx` / `SystemLogicalProcessorAndGroupInformation`, same value in source 28). Buffer too small yields failure with `ERROR_INSUFFICIENT_BUFFER` and a required length (source 1).
- **Reported records** (sources 1–4, 7, 26, 27, 29, 31):
  - Logical processor count matches the started set (and `KeQueryActiveProcessors` bit count for group 0, source 6).
  - One `RelationProcessorCore` record per core. Mask and `Flags` match source 27 for the three QEMU shapes (`flags=0`, single-bit masks). Learn still documents `LTP_PC_SMT` when a core has more than one logical processor (source 4); do not invent that flag when the log shows `0`. `EfficiencyClass` is 0 in those captures.
  - Package, cache, and exactly one NUMA node records for this single-group machine. Exact Windows cache lines are in section 6. Emit those Ex cache records even when leaf `0x4` is null and leaf `0x8000001D` is absent (source 33). Tavera source 29 currently emits no cache records.
  - One group 0 record. For the three source-27 shapes, `MaximumProcessorCount` equals `ActiveProcessorCount` (both 4) (sources 27, 31). Reporting `max=64` with `active=4` fails the differential (source 29).
  - `RelationProcessorDie` and `RelationProcessorModule` only when CPUID reports those levels (source 9 module/die level types). Source 27 measured none.
- **Live CD images** (sources 22, 23, 26). After a 2-CPU boot reaches the desktop, the live entries point at `ntkrnlmp.exe` and `halmp.dll`. Uniprocessor `ntoskrnl.exe` / `hal.dll` (and related) stay on the CD. `ONECPU` remains the escape.

## 5. Behavior

### Application processors

The boot processor runs `KiSystemStartup` as today. `KeStartAllProcessors` then starts each additional logical processor the HAL can launch, up to 64 and up to firmware/HAL presence (sources 12, 26). Each AP leaves real mode, enters long mode, then enters `KiSystemStartup` (source 26). After a successful `HalStartNextProcessor`, the starter waits until `KeLoaderBlock->Prcb` is cleared, matching the i386 loop in source 12. No finite timeout is specified. `HalStartNextProcessor` failure stops further starts and leaves already-started processors running. With `ONECPU`, `HalStartNextProcessor` returns `FALSE` immediately and only the boot processor runs (sources 14, 15).

### Topology

On Intel, prefer leaf `0x1F` when present, else leaf `0x0B` (sources 9, 26). SMT level type is `0x01` and core level type is `0x02`. Module and die level types from leaf `0x1F` drive optional die/module records only when those levels appear.

On AMD, read leaves `0x80000008` and `0x8000001E` when present (sources 10, 26). Record the returned fields; do not invent meanings beyond the in-tree header names and the guest log.

Fill each PRCB's `MultiThreadProcessorSet` with the Ex core mask for that logical processor so `GetLogicalProcessorInformationEx` matches source 27. On those QEMU shapes the mask is a single bit. Cap the machine at 64 logical processors. Do not store CPUID SMT sibling pairs in `MultiThreadProcessorSet` when Ex reports single-bit cores.

Ex cache records for these shapes are the section 6 lines (source 27). Source 33 shows leaf `0x4` null and leaf `0x8000001D` absent on the qemu64 shapes while Ex still lists those caches; on EPYC, `0x8000001D` sizes match Ex and leaf `0x4` stays null. Prefer emitting Ex caches that match the log. Do not omit Ex caches because a deterministic leaf is empty.

Learn documents `LTP_PC_SMT` when a core has more than one logical processor (source 4). Source 27 did not set that flag on the three QEMU shapes; see section 6.

### Scheduling preference

When two threads can run, ideal-processor selection uses the existing `~MultiThreadProcessorSet` preference (source 19). With Ex-matching single-bit sets, that prefers another logical processor, which source 27 treats as another core. Milestone “prefer two cores over two siblings” is evaluated against Ex core sets, not invented CPUID pairs. Source 27 `DIFF PLACE … shared_core=0` is consistent with that.

### IPI and TLB

`KeIpiGenericCall` matches source 5. TLB shootdown for all processors uses that broadcast path (or equivalent IPI) so every active CPU flushes. Delivery addresses targets by physical APIC ID; use x2APIC only when the ID does not fit in eight bits (sources 9, 16, 26). Cycle accounting for a running thread continues to use the processor it is on (source 25).

### Reporting

`GetLogicalProcessorInformationEx` with `RelationAll` returns the record kinds and fields in section 4. Exact masks, order, cache sizes, and Flags for the three QEMU shapes are the lines in source 27 (section 6). Source 33 shows Ex cache lines remain required when deterministic cache leaves are empty or absent; do not skip Ex caches because leaf `0x4` or `0x8000001D` is missing.

### Live CD switch

Do not point the live CD at `ntkrnlmp.exe` / `halmp.dll` until a 2-CPU boot has reached the desktop (source 26). Then switch the live entries. Rerun the milestone 8 syscall probe afterward.

## 6. Observed behavior

Windows 10 22H2 x64 on QEMU TCG (`-m 8192`, disk snapshot). Probe: `tavera/probes/amd64-logical-processors/probe-v1`. Each log below contains `DIFF END OK` (source 27).

### `-cpu qemu64 -smp 4,sockets=1,cores=4,threads=1` → `windows-4c1t.log`

- `DIFF LOGICAL 4`, `DIFF CORE_COUNT 4`
- Four `DIFF REL core` lines: `flags=0 eff=0`, masks `0x1`, `0x2`, `0x4`, `0x8`
- One package `mask=0xf`, one NUMA node 0 `mask=0xf`, one group (max 1, active 1) with `max=4 active=4 mask=0xf`
- Exact cache lines from the log:

```text
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=2 group=0 mask=0x1
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=1 group=0 mask=0x1
DIFF REL cache level=2 assoc=16 line=64 size=524288 type=0 group=0 mask=0x1
DIFF REL cache level=3 assoc=16 line=64 size=16777216 type=0 group=0 mask=0xf
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=2 group=0 mask=0x2
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=1 group=0 mask=0x2
DIFF REL cache level=2 assoc=16 line=64 size=524288 type=0 group=0 mask=0x2
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=2 group=0 mask=0x4
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=1 group=0 mask=0x4
DIFF REL cache level=2 assoc=16 line=64 size=524288 type=0 group=0 mask=0x4
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=2 group=0 mask=0x8
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=1 group=0 mask=0x8
DIFF REL cache level=2 assoc=16 line=64 size=524288 type=0 group=0 mask=0x8
```

- `DIFF EX_DIE_RECORDS 0`, `DIFF EX_MODULE_RECORDS 0`, `DIFF CPUID_DIE 0`, `DIFF CPUID_MODULE 0`
- CPUID topology leaf `0xb` (no `0x1F`); `DIFF PLACE … shared_core=0`
- Probe-v2 (source 33): `DIFF CPUID cache4 sub=0 type=null`; `DIFF CPUID amdcache absent max=0x8000000a`

### `-cpu qemu64 -smp 4,sockets=1,cores=2,threads=2` → `windows-qemu64-2c2t.log`

- `DIFF LOGICAL 4`, `DIFF CORE_COUNT 4` (not two cores)
- Four `DIFF REL core` lines with `flags=0` and single-bit masks `0x1`…`0x8` (no pair masks, no `LTP_PC_SMT`)
- Package / NUMA / group same shape as above (`mask=0xf`, four active processors)
- Exact cache lines from the log:

```text
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=2 group=0 mask=0x1
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=1 group=0 mask=0x1
DIFF REL cache level=2 assoc=16 line=64 size=524288 type=0 group=0 mask=0x1
DIFF REL cache level=3 assoc=16 line=64 size=16777216 type=0 group=0 mask=0xf
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=2 group=0 mask=0x2
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=1 group=0 mask=0x2
DIFF REL cache level=2 assoc=16 line=64 size=524288 type=0 group=0 mask=0x2
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=2 group=0 mask=0x4
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=1 group=0 mask=0x4
DIFF REL cache level=2 assoc=16 line=64 size=524288 type=0 group=0 mask=0x4
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=2 group=0 mask=0x8
DIFF REL cache level=1 assoc=2 line=64 size=65536 type=1 group=0 mask=0x8
DIFF REL cache level=2 assoc=16 line=64 size=524288 type=0 group=0 mask=0x8
```

- Die/module records still 0
- CPUID leaf `0xb`: sub 0 `shift=1 level_type=1` (SMT), sub 1 `shift=2 level_type=2` (core)
- `DIFF PLACE … shared_core=0`
- Probe-v2 (source 33): `DIFF CPUID cache4 sub=0 type=null`; `DIFF CPUID amdcache absent max=0x8000000a`

### `-cpu EPYC -smp 4,sockets=1,cores=2,threads=2` → `windows-epyc-2c2t.log`

- Same Ex core shape: `DIFF LOGICAL 4`, `DIFF CORE_COUNT 4`, four cores `flags=0` single-bit masks
- Exact cache lines from the log:

```text
DIFF REL cache level=1 assoc=8 line=64 size=32768 type=2 group=0 mask=0x1
DIFF REL cache level=1 assoc=4 line=64 size=65536 type=1 group=0 mask=0x1
DIFF REL cache level=2 assoc=8 line=64 size=524288 type=0 group=0 mask=0x1
DIFF REL cache level=3 assoc=16 line=64 size=8388608 type=0 group=0 mask=0x1
DIFF REL cache level=1 assoc=8 line=64 size=32768 type=2 group=0 mask=0x2
DIFF REL cache level=1 assoc=4 line=64 size=65536 type=1 group=0 mask=0x2
DIFF REL cache level=2 assoc=8 line=64 size=524288 type=0 group=0 mask=0x2
DIFF REL cache level=3 assoc=16 line=64 size=8388608 type=0 group=0 mask=0x2
DIFF REL cache level=1 assoc=8 line=64 size=32768 type=2 group=0 mask=0x4
DIFF REL cache level=1 assoc=4 line=64 size=65536 type=1 group=0 mask=0x4
DIFF REL cache level=2 assoc=8 line=64 size=524288 type=0 group=0 mask=0x4
DIFF REL cache level=3 assoc=16 line=64 size=8388608 type=0 group=0 mask=0x4
DIFF REL cache level=1 assoc=8 line=64 size=32768 type=2 group=0 mask=0x8
DIFF REL cache level=1 assoc=4 line=64 size=65536 type=1 group=0 mask=0x8
DIFF REL cache level=2 assoc=8 line=64 size=524288 type=0 group=0 mask=0x8
DIFF REL cache level=3 assoc=16 line=64 size=8388608 type=0 group=0 mask=0x8
```

- Die/module records 0
- CPUID leaf `0xb` with SMT `shift=1`; `amd80000008 nc=3 apic_core_id_size=2`; `amd8000001e core_id=0 threads_per_core=1 eax=0x0`
- `DIFF PLACE … shared_core=0`
- Probe-v2 (source 33): `DIFF CPUID cache4 sub=0 type=null`; `amdcache` subleaves 0–3 with sizes 32768 / 65536 / 524288 / 8388608 matching the Ex lines above; subleaf 4 `type=null`

The milestone success table (source 26) expected two cores with pair masks and `LTP_PC_SMT` on the `threads=2` shapes. Source 27 shows Windows 10 22H2 did not report that Ex shape under these QEMU guests. Reporting and `MultiThreadProcessorSet` must match source 27.

### Probe-v2 cache leaves (source 33)

Same three Windows guests as above. Ex `DIFF REL cache` lines match source 27. Leaf `0x4` is null on every shape. Leaf `0x8000001D` is absent on both qemu64 shapes while Ex still has caches; on EPYC it is present and its sizes match that shape’s Ex lines. Ex cache fill for the differential is the section 6 lines, not “decode leaf `0x4` or skip.”

### Tavera OS (same three shapes) — source 29

Each of `tavera-4c1t.log`, `tavera-qemu64-2c2t.log`, and `tavera-epyc-2c2t.log` contains `DIFF END OK`.

- `DIFF LOGICAL 4`, `DIFF CORE_COUNT 4`
- Four `DIFF REL core` lines: `flags=0`, single-bit masks `0x1`…`0x8` (matches Windows Ex cores)
- Package / NUMA / group present with `mask=0xf`
- **No** `DIFF REL cache` lines (Windows section 6 / source 33 require them)
- `DIFF REL groupinfo i=0 max=64 active=4 mask=0xf` — Windows source 27 has `max=4 active=4` (sources 27, 31)

## 7. Compatibility notes

Source 1 documents WOW64 affinity folding above 64 processors. This milestone stays in one group of at most 64, so that folding is out of scope.

Source 4 documents `LTP_PC_SMT` and nonzero `EfficiencyClass` on heterogeneous cores. Source 27 measured `flags=0` and `eff=0` on every core record for all three QEMU shapes.

Source 6 documents that `KeQueryActiveProcessors` returns group 0 on Windows 7 and later. This milestone is group 0 only. Source 27 shows one active group.

`SystemLogicalProcessorInformation` (source 20) already exposes core masks from `MultiThreadProcessorSet`. The Ex path (sources 1, 21) is what section 6 cites.

QEMU `threads=2` still exposes CPUID SMT level_type on leaf `0xb` in source 27 while Ex core records stay single-bit with `flags=0`. Do not invent Ex SMT pairing that the log does not show.

For the three QEMU shapes, `PROCESSOR_GROUP_INFO.MaximumProcessorCount` equals `ActiveProcessorCount` (both 4) on Windows (sources 27, 31). Source 29’s `max=64` is a Tavera mismatch.

## 8. Test plan

These checks confirm behavior. They do not prescribe coding steps beyond sections 4 and 5.

1. **Topology probe.** `tavera/probes/amd64-logical-processors/probe-v1` already produced Windows (source 27) and Tavera (source 29) logs. Probe-v2 (source 33) added Windows leaf `0x4` / `0x8000001D` dumps. After cache and group-`max` fixes, re-run Tavera; require `DIFF END OK` and match source 27 including cache lines and `groupinfo max=4`.
2. **Placement.** Source 27 already recorded `DIFF PLACE … shared_core=0` on Windows for each shape. Source 29 also recorded `shared_core=0`. Re-run after changes if needed.
3. **`ONECPU`.** Boot with `ONECPU` and confirm only the boot processor is active and the desktop still appears.
4. **2-CPU gate then MP images.** Boot with two CPUs to the desktop on `ntkrnlmp`/`halmp` (or the interim images used to prove the gate). Then point the live CD at `ntkrnlmp.exe` and `halmp.dll`, rebuild, and boot `-smp 4`.
5. **Milestone 8 probe.** Rerun `buildenv/differential-tests/syscall-diff` (or the tree's current path) so the recorded statuses do not regress. Reuse the Windows log when `Test-WindowsProbe.ps1` prints `REUSE`.
6. **Desktop.** Every success boot still reaches the desktop before the log is treated as success. Interactive QEMU: name the window, what ready looks like, and when to report back. One QEMU on a disk at a time.

## 9. Open questions

- Windows APIC delivery by physical APIC ID past eight processors remains unobserved with allowed probes (ALP-005). Tavera smp9 (source 30) is implementation evidence only.

## 10. Review log

Written 2026-11-18. Section 6 filled from probe-v1 Windows captures the same day. 2026-11-18 later: ALP-002/006/007/008 resolved (i386 wait, Ex-matching `MultiThreadProcessorSet`, exact cache DIFF lines, class 107 dual names). 2026-11-19: cited Tavera probe-v1 (source 29), smp9 (source 30), group `MaximumProcessorCount` (source 31), opened ALP-009 for cache leaves; softened x2APIC / stub source blurbs. 2026-11-19 later: probe-v2 source 33; ALP-009 resolved (Ex caches required when leaf `0x4` null / `0x8000001D` absent; EPYC `0x8000001D` sizes match Ex). A reviewer still has to confirm this file has no implementation detail from a forbidden source.
