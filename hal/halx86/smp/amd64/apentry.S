/*
 * PROJECT:     Tavera OS Kernel
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        hal/halx86/smp/amd64/apentry.S
 * PURPOSE:     AMD64 Application Processor (AP) spinup trampoline
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Justin Miller <justin.miller@reactos.org>
 *              Ryan Wilson <ryan@taveraos.com>
 *
 * Sources: in-tree hal/halx86/smp/i386/apentry.S; boot/freeldr/freeldr/arch/realmode/amd64.S
 * (ExitToLongMode / gdt long-mode CS); ntoskrnl/include/internal/amd64/ke.h (EFER_LME,
 * CR0_PE/CR0_PG, CR4_PAE); tavera/specs/amd64-logical-processors/amd64-logical-processors.md;
 * tavera/specs/amd64-logical-processors/debugging-2026-11-19-whpx.md (WHPX Gate B IA vs
 * TCG IA1234B…; real-mode DS==CS).
 *
 * Identity map brings the AP into long mode at a physical RIP. Kernel CR3 drops that
 * map, so the trampoline jumps to HalpLowStub (kernel VA of the same pages) before
 * loading the BSP CR3, then continues from that VA which stays mapped.
 */

#include <asm.inc>
#include <ksamd64.inc>

#define ZERO_OFFSET(f) (f - HalpAPEntry16)

PUBLIC HalpAPEntry16
PUBLIC HalpAPEntryData
PUBLIC HalpAPEntry32
PUBLIC HalpAPEntry64
PUBLIC HalpAPEntry16End

.code16
HalpAPEntry16:
    cli

    /*
     * Real-mode DS/ES/SS must equal CS so stub-relative loads stay within the
     * 64 KiB segment limit. DS=0 with [ebp+off] (ebp = CS<<4) uses an offset
     * above 64 KiB; TCG often allows that, WHPX/Hyper-V does not (Gate B:
     * debugcon IA then stop before post-lgdt '1'; TCG control IA1234B…).
     * Same CS-segment pattern as hal/halx86/smp/i386/apentry.S.
     *
     * Still compute ebp = CS<<4: HalpAPEntry64 uses rbp for identity-mapped
     * loads of LoaderBlock/ProcessorState/StubKernelVa before the VA jump.
     * W2f without ebp stopped at debugcon C (`WHPX: Unexpected VP exit code 4`).
     */
    mov ebp, cs
    shl ebp, 4

    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax

    /* QEMU isa-debugcon (iobase 0xe9): breadcrumb through the trampoline */
    mov al, HEX(41) /* 'A' */
    out HEX(0E9), al

    /*
     * Stub-relative GDTR (limit+physical base patched in spinup.c). Do not use
     * GAS `cs:[imm]` MOV forms here — they can drop the override (Phase B3);
     * with DS==CS the plain displacement is enough.
     */
#ifdef _USE_ML
    data32 lgdt fword ptr [ZERO_OFFSET(TempGdtr)]
#else
    data32 lgdt [ZERO_OFFSET(TempGdtr)]
#endif

    /* WHPX Gate B: sub-steps A→1→2→3→4→B */
    mov al, HEX(31) /* '1' — after lgdt */
    out HEX(0E9), al

    /* Temporary page tables: identity for this stub + BSP kernel PML4 copy */
    mov eax, [ZERO_OFFSET(PageTableRoot)]
    mov cr3, eax

    mov al, HEX(32) /* '2' — after CR3 */
    out HEX(0E9), al

    /* CR4: PAE | PGE (freeldr ExitToLongMode) */
    mov eax, cr4
    or eax, HEX(00A0)
    mov cr4, eax

    mov al, HEX(33) /* '3' — after CR4 */
    out HEX(0E9), al

    /* EFER.LME */
    mov ecx, MSR_EFER
    rdmsr
    or eax, MSR_LME
    wrmsr

    mov al, HEX(34) /* '4' — after EFER.LME */
    out HEX(0E9), al

    /* Enable paging and protection together */
    mov eax, cr0
    or eax, HEX(80000001)
    mov cr0, eax

    mov al, HEX(42) /* 'B' — about to enter long mode */
    out HEX(0E9), al

.align 4
    /* Far jump into 64-bit identity code: 66 EA offs32 seg16 */
    .byte HEX(66)
    .byte HEX(EA)
HalpAPEntryData:
Jump64Offset:
    .long 0
Jump64Segment:
    .long HEX(10)
PageTableRoot:
    .long 0
    .long 0
ProcessorState:
    .quad 0
LoaderBlock:
    .quad 0
StubKernelVa:
    .quad 0
TempGdtrPad:
    .short 0
TempGdtr:
    .short (TempGdtEnd - TempGdt - 1)
TempGdtrBase:
    .long 0
TempGdt:
    .word HEX(0000), HEX(0000), HEX(0000), HEX(0000) /* 00: NULL */
    .word HEX(0000), HEX(0000), HEX(0000), HEX(0000) /* 08: unused */
    .word HEX(0000), HEX(0000), HEX(9800), HEX(0020) /* 10: long mode CS */
    .word HEX(FFFF), HEX(0000), HEX(F300), HEX(00CF) /* 18: data */
TempGdtEnd:

.code64
HalpAPEntry64:
    mov al, HEX(43) /* 'C' — 64-bit identity entry */
    out HEX(0E9), al

    /*
     * Capture pointers while RIP is still identity-mapped, then jump to the
     * same stub bytes via HalpLowStub so the BSP CR3 can be loaded next.
     */
    mov r8, qword ptr [rbp + ZERO_OFFSET(LoaderBlock)]
    mov rsi, qword ptr [rbp + ZERO_OFFSET(ProcessorState)]
    mov rax, qword ptr [rbp + ZERO_OFFSET(StubKernelVa)]
    add rax, ZERO_OFFSET(HalpAPEntry64High)
    jmp rax

HalpAPEntry64High:
    mov al, HEX(44) /* 'D' — HalpLowStub VA */
    out HEX(0E9), al

    /* Real kernel page tables (HalpLowStub stays mapped; identity may go away). */
    mov rax, qword ptr [rsi + PsCr3]
    mov cr3, rax

    mov al, HEX(45) /* 'E' — BSP CR3 */
    out HEX(0E9), al

    mov rax, qword ptr [rsi + PsCr4]
    mov cr4, rax
    mov rax, qword ptr [rsi + PsCr0]
    mov cr0, rax

    /* Also enable NXE so kernel PTEs that set NX are interpreted correctly. */
    mov ecx, MSR_EFER
    rdmsr
    or eax, HEX(900) /* MSR_LME | MSR_NXE — LME already set; OR NXE (0x800) */
    wrmsr

    mov al, HEX(46) /* 'F' — CR4/CR0/EFER */
    out HEX(0E9), al

#ifdef _USE_ML
    lgdt fword ptr [rsi + PsGdtr + 6]
    lidt fword ptr [rsi + PsIdtr + 6]
#else
    lgdt [rsi + PsGdtr + 6]
    lidt [rsi + PsIdtr + 6]
#endif
    ltr word ptr [rsi + PsTr]

    mov al, HEX(47) /* 'G' — GDT/IDT/TR */
    out HEX(0E9), al

    /* Real GDT is loaded; fix SS/DS (still 0 from real mode) before retfq. */
    mov ax, KGDT64_R0_DATA
    mov ss, ax
    mov ds, ax
    mov es, ax

    mov rsp, qword ptr [rsi + PsContextFrame + CxRsp]
    /* Match CALL alignment: RSP must be 8 mod 16 at function entry. */
    and rsp, HEX(0FFFFFFFFFFFFFFF0)
    sub rsp, 8

    mov rcx, r8
    mov rax, qword ptr [rsi + PsContextFrame + CxRip]

    /* Show that CxRip looks like a high canonical kernel VA (byte 7 == 0xFF). */
    push rax
    shr rax, 56
    out HEX(0E9), al
    pop rax

    xor edx, edx
    xor ebx, ebx
    xor ebp, ebp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    xor esi, esi
    xor edi, edi

    mov r10, rax
    mov al, HEX(48) /* 'H' — near jmp into KiSystemStartup */
    out HEX(0E9), al
    mov rax, r10

    jmp rax

HalpAPEntry16End:

HalpAPEntry32:
    hlt

END
