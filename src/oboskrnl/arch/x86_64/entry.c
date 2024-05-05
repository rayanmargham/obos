/*
*	oboskrnl/arch/x86_64/entry.c
*
*	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <struct_packing.h>
#include <memmanip.h>
#include <text.h>
#include <font.h>

#include <UltraProtocol/ultra_protocol.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/pmm.h>

#include <allocators/basic_allocator.h>

#include <arch/x86_64/boot_info.h>

extern void Arch_InitBootGDT();

static char thr_stack[0x4000];
static char kmain_thr_stack[0x10000];
extern char Arch_InitialISTStack[0x10000];
extern void Arch_disablePIC();
static thread bsp_idleThread;
static thread_node bsp_idleThreadNode;

static thread kernelMainThread;
static thread_node kernelMainThreadNode;

static cpu_local bsp_cpu;
__asm__(
	".global Arch_IdleTask; Arch_IdleTask:; hlt; jmp Arch_IdleTask;"
);
extern void Arch_IdleTask();
extern void Arch_FlushGDT(uintptr_t gdtr);
void Arch_KernelMainBootstrap(struct ultra_boot_context* bcontext);
static void ParseBootContext(struct ultra_boot_context* bcontext);
void pageFaultHandler(interrupt_frame* frame);
void Arch_KernelEntry(struct ultra_boot_context* bcontext, uint32_t magic)
{
	if (magic != ULTRA_MAGIC)
		return; // All hope is lost.
	// TODO: Parse boot context.
	// This call will ensure the IRQL is at the default IRQL (IRQL_MASKED).
	Arch_disablePIC();
	Core_GetIrql();
	asm("sti");
	OBOS_Debug("%s: Initializing the Boot GDT.\n", __func__);
	Arch_InitBootGDT();
	OBOS_Debug("%s: Initializing the Boot IDT.\n", __func__);
	Arch_RawRegisterInterrupt(0xe, pageFaultHandler);
	ParseBootContext(bcontext);
	if (Arch_LdrPlatformInfo->page_table_depth != 4)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "5-level paging is unsupported by oboskrnl.\n");
	if (!Arch_Framebuffer)
		OBOS_Warning("No framebuffer passed by the framebuffer. All kernel logs will be on port 0xE9.\n");
	else
	{
		OBOS_TextRendererState.fb.base = Arch_MapToHHDM(Arch_Framebuffer->physical_address);
		OBOS_TextRendererState.fb.bpp = Arch_Framebuffer->bpp;
		OBOS_TextRendererState.fb.format = Arch_Framebuffer->format;
		OBOS_TextRendererState.fb.height = Arch_Framebuffer->height;
		OBOS_TextRendererState.fb.width = Arch_Framebuffer->width;
		OBOS_TextRendererState.fb.pitch = Arch_Framebuffer->pitch;
		OBOS_TextRendererState.column = 0;
		OBOS_TextRendererState.row = 0;
		OBOS_TextRendererState.font = font_bin;
	}
	Arch_InitializeIDT();
	OBOS_Debug("Enabling XD bit in IA32_EFER.\n");
	{
		uint32_t edx = 0;
		__cpuid__(0x80000001, 0, nullptr, nullptr, nullptr, &edx);
		if (edx & (1 << 20))
			wrmsr(0xC0000080 /* IA32_EFER */, rdmsr(0xC0000080) | (1<<11) /* XD Enable */);
	}
	OBOS_Debug("%s: Initializing scheduler.\n", __func__);
	bsp_cpu.id = 0;
	bsp_cpu.isBSP = true;
	Core_CpuCount = 1;
	Core_CpuInfo = &bsp_cpu;
	thread_ctx ctx1, ctx2;
	memzero(&ctx1, sizeof(ctx1));
	memzero(&ctx2, sizeof(ctx2));
	OBOS_Debug("Setting up thread context.\n");
	CoreS_SetupThreadContext(&ctx2, Arch_KernelMainBootstrap, bcontext, false, kmain_thr_stack, 0x10000);
	CoreS_SetupThreadContext(&ctx1, Arch_IdleTask, 0, false, thr_stack, 0x4000);
	OBOS_Debug("Initializing thread.\n");
	CoreH_ThreadInitialize(&kernelMainThread, THREAD_PRIORITY_NORMAL, 1, &ctx2);
	CoreH_ThreadInitialize(&bsp_idleThread, THREAD_PRIORITY_IDLE, 1, &ctx1);
	kernelMainThread.context.gs_base = &bsp_cpu;
	bsp_idleThread.context.gs_base = &bsp_cpu;
	OBOS_Debug("Readying threads for execution.\n");
	CoreH_ThreadReadyNode(&kernelMainThread, &kernelMainThreadNode);
	CoreH_ThreadReadyNode(&bsp_idleThread, &bsp_idleThreadNode);
	OBOS_Debug("Initializing CPU-local GDT.\n");
	// Initialize the CPU's GDT.
	memzero(Core_CpuInfo[0].arch_specific.gdtEntries, sizeof(Core_CpuInfo[0].arch_specific.gdtEntries));
	memzero(&Core_CpuInfo[0].arch_specific.tss, sizeof(Core_CpuInfo[0].arch_specific.tss));
	Core_CpuInfo[0].arch_specific.gdtEntries[0] = 0;
	Core_CpuInfo[0].arch_specific.gdtEntries[1] = 0x00af9b000000ffff; // 64-bit code
	Core_CpuInfo[0].arch_specific.gdtEntries[2] = 0x00cf93000000ffff; // 64-bit data
	Core_CpuInfo[0].arch_specific.gdtEntries[3] = 0x00aff3000000ffff; // 64-bit user-mode data
	Core_CpuInfo[0].arch_specific.gdtEntries[4] = 0x00affb000000ffff; // 64-bit user-mode code
	struct
	{
		uint16_t limitLow;
		uint16_t baseLow;
		uint8_t baseMiddle1;
		uint8_t access;
		uint8_t gran;
		uint8_t baseMiddle2;
		uint32_t baseHigh;
		uint32_t resv1;
	} tss_entry;
	memzero(&tss_entry, sizeof(tss_entry));
	uintptr_t tss = (uintptr_t)&Core_CpuInfo[0].arch_specific.tss;
	tss_entry.limitLow = sizeof(Core_CpuInfo[0].arch_specific.tss);
	tss_entry.baseLow = tss & 0xffff;
	tss_entry.baseMiddle1 = (tss >> 16) & 0xff;
	tss_entry.baseMiddle2 = (tss >> 24) & 0xff;
	tss_entry.baseHigh = (tss >> 32) & 0xffffffff;
	tss_entry.access = 0x89;
	tss_entry.gran = 0x40;
	Core_CpuInfo[0].arch_specific.gdtEntries[5] = *((uint64_t*)&tss_entry + 0);
	Core_CpuInfo[0].arch_specific.gdtEntries[6] = *((uint64_t*)&tss_entry + 1);

	Core_CpuInfo[0].arch_specific.tss.ist0 = Arch_InitialISTStack + sizeof(Arch_InitialISTStack);
	Core_CpuInfo[0].arch_specific.tss.rsp0 = Arch_InitialISTStack + sizeof(Arch_InitialISTStack);
	Core_CpuInfo[0].arch_specific.tss.iopb = sizeof(Core_CpuInfo[0].arch_specific.tss) - 1;
	struct
	{
		uint16_t limit;
		uintptr_t base;
	} OBOS_PACK gdtr;
	gdtr.limit = sizeof(Core_CpuInfo[0].arch_specific.gdtEntries) - 1;
	gdtr.base = Core_CpuInfo[0].arch_specific.gdtEntries;
	Arch_FlushGDT(&gdtr);
	OBOS_Debug("Initializing GS_BASE.\n");
	wrmsr(0xC0000101, (uintptr_t)&Core_CpuInfo[0]);
	Core_CpuInfo[0].currentIrql = Core_GetIrql();
	OBOS_Debug("Initializing priority lists.\n");
	for (thread_priority i = 0; i <= THREAD_PRIORITY_MAX_VALUE; i++)
		Core_CpuInfo[0].priorityLists[i].priority = i;
	// Finally yield into the scheduler.
	OBOS_Debug("Yielding into the scheduler!\n");
	Core_Yield();
	OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Scheduler did not switch to a new thread.\n");
	while (1);
}
struct ultra_memory_map_attribute* Arch_MemoryMap;
struct ultra_platform_info_attribute* Arch_LdrPlatformInfo;
struct ultra_kernel_info_attribute* Arch_KernelInfo;
struct ultra_module_info_attribute* Arch_KernelBinary;
struct ultra_framebuffer* Arch_Framebuffer;
const char* OBOS_KernelCmdLine;
static void ParseBootContext(struct ultra_boot_context* bcontext)
{
	struct ultra_attribute_header* header = bcontext->attributes;
	for (size_t i = 0; i < bcontext->attribute_count; i++, header = ULTRA_NEXT_ATTRIBUTE(header))
	{
		switch (header->type)
		{
		case ULTRA_ATTRIBUTE_PLATFORM_INFO: Arch_LdrPlatformInfo = header; break;
		case ULTRA_ATTRIBUTE_KERNEL_INFO: Arch_KernelInfo = header;  break;
		case ULTRA_ATTRIBUTE_MEMORY_MAP: Arch_MemoryMap = header; break;
		case ULTRA_ATTRIBUTE_COMMAND_LINE: OBOS_KernelCmdLine = (header + 1); break;
		case ULTRA_ATTRIBUTE_FRAMEBUFFER_INFO: 
		{
			struct ultra_framebuffer_attribute* fb = header;
			Arch_Framebuffer = &fb->fb;
			break;
		}
		case ULTRA_ATTRIBUTE_MODULE_INFO: 
		{
			struct ultra_module_info_attribute* module = header;
			if (strcmp(module->name, "__KERNEL__"))
				Arch_KernelBinary = module;
			break;
		}
		case ULTRA_ATTRIBUTE_INVALID:
			OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Invalid UltraProtocol attribute type %d.\n", header->type);
			break;
		default:
			break;
		}
	}
	if (!Arch_LdrPlatformInfo)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find platform info from bootloader.\n");
	if (Arch_LdrPlatformInfo->platform_type == ULTRA_PLATFORM_INVALID)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Invalid platform type %d.\n", Arch_LdrPlatformInfo->platform_type);
	if (!Arch_KernelInfo)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find kernel info from bootloader.\n");
	if (Arch_KernelInfo->partition_type == ULTRA_PARTITION_TYPE_INVALID)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Invalid partition type %d.\n", Arch_KernelInfo->partition_type);
	if (!Arch_KernelBinary)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find kernel module in boot context!\nDo you set kernel-as-module to true in the hyper.cfg?\n");
}
extern obos_status Arch_InitializeKernelPageTable();
static size_t runAllocatorTests(allocator_info* allocator, size_t passes);
void pageFaultHandler(interrupt_frame* frame)
{
	OBOS_Panic(OBOS_PANIC_EXCEPTION, 
		"Page fault at 0x%p in %s-mode while to %s page at 0x%p, which is %s. Error code: %d\n"
		"Register dump:\n"
		"\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
		"\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
		"\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
		"\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
		"\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
		"\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
		"\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
		"\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
		"\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n",
		(void*)frame->rip,
		frame->cs == 0x8 ? "kernel" : "user",
		(frame->errorCode & 2) ? "write" : ((frame->errorCode & 0x10) ? "execute" : "read"),
		getCR2(),
		frame->errorCode & 1 ? "present" : "unpresent",
		frame->errorCode,
		frame->rdi, frame->rsi, frame->rbp,
		frame->rsp, frame->rbx, frame->rdx,
		frame->rcx, frame->rax, frame->rip,
		frame->r8, frame->r9, frame->r10,
		frame->r11, frame->r12, frame->r13,
		frame->r14, frame->r15, frame->rflags,
		frame->ss, frame->ds, frame->cs,
		getCR0(), getCR2(), getCR3(),
		getCR4(), Core_GetIrql(), getEFER()
	);
}
uint64_t random_number();
__asm__(
	".global random_number; random_number:; rdrand %rax; ret;"
);
void Arch_KernelMainBootstrap(struct ultra_boot_context* bcontext)
{
	//Core_Yield();
	OBOS_Debug("%s: Initializing PMM.\n", __func__);
	Arch_InitializePMM();
	OBOS_Debug("%s: Initializing page tables.\n", __func__);
	obos_status status = Arch_InitializeKernelPageTable();
	if (status != OBOS_STATUS_SUCCESS)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize page tables. Status: %d.\n", status);
	OBOS_Debug("%s: Testing allocator...\n", __func__);
	basic_allocator alloc;
	OBOSH_ConstructBasicAllocator(&alloc);
	OBOS_ASSERT(runAllocatorTests(&alloc, 1000000) == 1000000);
	OBOS_Log("%s: Done early boot.\n", __func__);
	while (1);
}
static size_t runAllocatorTests(allocator_info* allocator, size_t passes)
{
	OBOS_Debug("%s: Testing allocator. Pass count is %lu.\n", __func__, passes);
	void* lastDiv16Pointer = nullptr;
	size_t passInterval = 10000;
	if (passInterval % 10)
		passInterval += (passInterval - passInterval % 10);
	size_t lastStatusMessageInterval = 0;
	const char* buf = "\xef\xbe\xad\xed";
	size_t lastFreeIndex = 0;
	for (size_t i = 0; i < passes; i++)
	{
		if (!i)
			OBOS_Debug("%s: &i=0x%p\n", __func__, &i);
		if ((lastStatusMessageInterval + passInterval) == i)
		{
			OBOS_Debug("%s: Finished %lu passes so far.\n", __func__, i);
			lastStatusMessageInterval = i;
		}
		uint64_t r = random_number();
		void* mem = allocator->Allocate(allocator, r % 0x10000 + 16, nullptr);
		if (!mem)
			return i;
		((uint8_t*)mem)[i % 4] = buf[i % 4];
		if (++lastFreeIndex == 3)
		{
			lastFreeIndex = 0;
			if (lastDiv16Pointer)
			{
				size_t objSize = allocator->QueryBlockSize(allocator, lastDiv16Pointer, nullptr);
				if (objSize == SIZE_MAX)
					return i;
				allocator->Free(allocator, lastDiv16Pointer, objSize);
			}
			lastDiv16Pointer = mem;
		}
	}
	return passes;
}