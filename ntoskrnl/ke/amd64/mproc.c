/*
 * PROJECT:     Tavera OS Kernel
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        ntoskrnl/ke/amd64/mproc.c
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
 * PROGRAMMERS: Justin Miller <justin.miller@reactos.org>
 *              Ryan Wilson <ryan@taveraos.com>
 *
 * Sources: in-tree ntoskrnl/ke/i386/mproc.c; ntoskrnl/ke/amd64/kiinit.c
 * (KiInitializeProcessorBootStructures, KiSystemStartup); sdk/include/ndk/amd64/ketypes.h
 * (KGDT64_*); tavera/specs/amd64-logical-processors/amd64-logical-processors.md;
 * open-items ALP-002 (reuse i386 infinite wait until a finite timeout is cited).
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY64 Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) UCHAR Gdt[PAGE_SIZE];
    DECLSPEC_ALIGN(16) UCHAR DoubleFaultStack[KERNEL_STACK_SIZE];
    DECLSPEC_ALIGN(16) UCHAR NmiStack[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS64 Tss;
} APINFO, *PAPINFO;

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack = NULL;
    PVOID DpcStack = NULL;
    PVOID RawAPInfo = NULL;
    PAPINFO APInfo = NULL;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;
    KDESCRIPTOR BspGdt, BspIdt;
    PKPROCESSOR_STATE ProcessorState;

    MaximumProcessors = KeMaximumProcessors;

    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);

    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    if (MaximumProcessors > MAXIMUM_PROC_PER_GROUP)
        MaximumProcessors = MAXIMUM_PROC_PER_GROUP;

    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        KernelStack = NULL;
        DpcStack = NULL;
        RawAPInfo = NULL;
        APInfo = NULL;

        /*
         * IDT/GDT need page alignment. Pool does not honor DECLSPEC_ALIGN, so
         * over-allocate and align like a page-sized object.
         */
        RawAPInfo = ExAllocatePoolZero(NonPagedPool,
                                       sizeof(*APInfo) + PAGE_SIZE,
                                       TAG_KERNEL);
        if (!RawAPInfo)
            break;

        APInfo = (PAPINFO)ALIGN_UP_POINTER_BY(RawAPInfo, PAGE_SIZE);

        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (!KernelStack)
            break;

        DpcStack = MmCreateKernelStack(FALSE, 0);
        if (!DpcStack)
            break;

        __sgdt(&BspGdt.Limit);
        __sidt(&BspIdt.Limit);

        ASSERT(BspGdt.Limit + 1U <= sizeof(APInfo->Gdt));
        ASSERT(BspIdt.Limit + 1U <= sizeof(APInfo->Idt));

        RtlCopyMemory(APInfo->Gdt, BspGdt.Base, BspGdt.Limit + 1);
        RtlCopyMemory(APInfo->Idt, BspIdt.Base, BspIdt.Limit + 1);

        KiInitializeProcessorBootStructures(ProcessorCount,
                                            &APInfo->Pcr,
                                            (PKGDTENTRY64)APInfo->Gdt,
                                            APInfo->Idt,
                                            &APInfo->Tss,
                                            &APInfo->Thread.Tcb,
                                            KernelStack,
                                            DpcStack,
                                            &APInfo->DoubleFaultStack[KERNEL_STACK_SIZE],
                                            &APInfo->NmiStack[DOUBLE_FAULT_STACK_SIZE]);

        ProcessorState = &APInfo->Pcr.Prcb.ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();
        ProcessorState->SpecialRegisters.Cr8 = __readcr8();

        ProcessorState->ContextFrame.SegCs = KGDT64_R0_CODE;
        ProcessorState->ContextFrame.SegDs = KGDT64_R3_DATA | RPL_MASK;
        ProcessorState->ContextFrame.SegEs = KGDT64_R3_DATA | RPL_MASK;
        ProcessorState->ContextFrame.SegSs = KGDT64_R0_DATA;
        ProcessorState->ContextFrame.SegFs = KGDT64_R3_CMTEB | RPL_MASK;
        ProcessorState->ContextFrame.SegGs = 0;

        ProcessorState->SpecialRegisters.Gdtr.Base = APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = BspGdt.Limit;
        ProcessorState->SpecialRegisters.Idtr.Base = APInfo->Idt;
        ProcessorState->SpecialRegisters.Idtr.Limit = BspIdt.Limit;
        ProcessorState->SpecialRegisters.Tr = KGDT64_SYS_TSS;

        ProcessorState->ContextFrame.Rsp = (ULONG64)KernelStack;
        ProcessorState->ContextFrame.Rip = (ULONG64)KiSystemStartup;
        ProcessorState->ContextFrame.EFlags = __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)&APInfo->Pcr.Prcb;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread;

        DPRINT1("KeStartAllProcessors: starting CPU %lu\n", ProcessorCount);
        /*
         * ALP-002: reuse the in-tree i386 wait (no finite timeout cited yet).
         * Spec asks for a timeout; inventing a duration is not allowed.
         * Print waiting before SIPI so a triple-fault during spin-up still
         * leaves a breadcrumb on the serial log.
         */
        DPRINT1("KeStartAllProcessors: waiting for CPU %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            DPRINT1("KeStartAllProcessors: HalStartNextProcessor failed for CPU %lu\n",
                    ProcessorCount);
            break;
        }

        {
            ULONG WaitSpins = 0;
            while (ReadULongPtrNoFence(&KeLoaderBlock->Prcb) != 0)
            {
                KeMemoryBarrier();
                YieldProcessor();
                if ((++WaitSpins & 0xFFFFF) == 0)
                {
                    DPRINT1("KeStartAllProcessors: still waiting CPU %lu Prcb=%p\n",
                            ProcessorCount,
                            (PVOID)KeLoaderBlock->Prcb);
                }
            }
        }
        DPRINT1("KeStartAllProcessors: CPU %lu joined\n", ProcessorCount);

        /* Retention: successful AP owns RawAPInfo/stacks; drop locals for next round */
        RawAPInfo = NULL;
        APInfo = NULL;
        KernelStack = NULL;
        DpcStack = NULL;
    }

    if (RawAPInfo)
        ExFreePoolWithTag(RawAPInfo, TAG_KERNEL);
    if (KernelStack)
        MmDeleteKernelStack(KernelStack, FALSE);
    if (DpcStack)
        MmDeleteKernelStack(DpcStack, FALSE);

    DPRINT1("KeStartAllProcessors: active processor count is %lu\n", KeNumberProcessors);
}
