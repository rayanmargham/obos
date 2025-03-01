/*
 * oboskrnl/sanitizers/asan.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <allocators/basic_allocator.h>

#include <sanitizers/asan.h>

#include <mm/context.h>

#define round_down_to_page(addr) ((uintptr_t)(addr) - ((uintptr_t)(addr) % OBOS_PAGE_SIZE))
// #define OBOS_CROSSES_PAGE_BOUNDARY(base, size) (round_down_to_page(base) == round_down_to_page((uintptr_t)(base) + (size)))

#ifdef __x86_64__
#include <arch/x86_64/asm_helpers.h>
uintptr_t Arch_GetPML4Entry(uintptr_t pml4Base, uintptr_t addr);
uintptr_t Arch_GetPML3Entry(uintptr_t pml4Base, uintptr_t addr);
uintptr_t Arch_GetPML2Entry(uintptr_t pml4Base, uintptr_t addr);
uintptr_t Arch_GetPML1Entry(uintptr_t pml4Base, uintptr_t addr);
uintptr_t* Arch_AllocatePageMapAt(uintptr_t pml4Base, uintptr_t at, uintptr_t cpuFlags, uint8_t depth);
bool Arch_FreePageMapAt(uintptr_t pml4Base, uintptr_t at, uint8_t maxDepth);
obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags);
obos_status Arch_MapHugePage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags);
#endif

const uint8_t OBOS_ASANPoisonValues[] = {
	0xDE,
	0xAA,
};
OBOS_NO_KASAN void asan_report(uintptr_t addr, size_t sz, uintptr_t ip, bool rw, asan_violation_type type, bool unused)
{
	OBOS_UNUSED(unused);
	switch (type)
	{
	case ASAN_InvalidAccess:
		OBOS_Panic(OBOS_PANIC_KASAN_VIOLATION, "ASAN Violation at %p while trying to %s %lu bytes from 0x%p.\n", (void*)ip, rw ? "write" : "read", sz, (void*)addr);
		break;
	case ASAN_ShadowSpaceAccess:
		OBOS_Panic(OBOS_PANIC_KASAN_VIOLATION, "ASAN Violation at %p while trying to %s %lu bytes from 0x%p (Hint: Pointer is in shadow space).\n", (void*)ip, rw ? "write" : "read", sz, (void*)addr);
		break;
	case ASAN_UseAfterFree:
		OBOS_Panic(OBOS_PANIC_KASAN_VIOLATION, "ASAN Violation at %p while trying to %s %lu bytes from 0x%p (Hint: Use of memory block after free).\n", (void*)ip, rw ? "write" : "read", sz, (void*)addr);
		break;
	default:
		break;
	}
}
static OBOS_NO_KASAN void asan_shadow_space_access(uintptr_t at, size_t size, uintptr_t ip, bool rw, uint8_t poisonIndex, bool abort)
{
	OBOS_ASSERT(poisonIndex <= ASAN_POISON_MAX);
	// Verify this memory is actually poisoned and not just coincidentally set to the poison.
	// Note: This method might not report all shadow space accesses.
	// First check 16 bytes after and before the pointer, and if either sides are poisoned, it is safe to assume that this access is that of the shadow space.
	bool isPoisoned = false;
	bool shortCircuitedFirst = false, shortCircuitedSecond = false;
	if (OBOS_CROSSES_PAGE_BOUNDARY(at - 16, 16))
		if ((shortCircuitedFirst = !KASAN_IsAllocated(at - 16, 16, false)))
			goto short_circuit1;
	isPoisoned = memcmp_b((void*)(at - 16), OBOS_ASANPoisonValues[poisonIndex], 16);
	short_circuit1:
	if (!isPoisoned)
		if (OBOS_CROSSES_PAGE_BOUNDARY(at + size, 16))
			if ((shortCircuitedSecond = !KASAN_IsAllocated(at + size, 16, false)))
				goto short_circuit2;
	isPoisoned = memcmp_b((void*)(at + size), OBOS_ASANPoisonValues[poisonIndex], 16);
short_circuit2:
	if (isPoisoned || (shortCircuitedFirst && shortCircuitedSecond))
	{
		asan_violation_type type = ASAN_InvalidType;
		switch (poisonIndex)
		{
		case ASAN_POISON_ALLOCATED:
			type = ASAN_ShadowSpaceAccess;
			break;
		case ASAN_POISON_FREED:
			type = ASAN_UseAfterFree;
			break;
		default:
			break;
		}
		asan_report(at, size, ip, rw, type, abort);
	}
}
OBOS_NO_KASAN static void asan_verify(uintptr_t at, size_t size, uintptr_t ip, bool rw, bool abort)
{
	/*if (!KASAN_IsAllocated(at, size, rw))
		asan_report(at, size, ip, rw, InvalidAccess, abort);*/
#ifdef __x86_64__
	// Make sure the pointer is valid.
	if (!(((uintptr_t)(at) >> 47) == 0 || ((uintptr_t)(at) >> 47) == 0x1ffff))
		asan_report(at, size, ip, rw, ASAN_InvalidAccess, abort);
#endif
	if (memcmp_b((void*)at, OBOS_ASANPoisonValues[ASAN_POISON_ALLOCATED], size))
		asan_shadow_space_access(at, size, ip, rw, ASAN_POISON_ALLOCATED, abort);
	if (memcmp_b((void*)at, OBOS_ASANPoisonValues[ASAN_POISON_FREED], size))
		asan_shadow_space_access(at, size, ip, rw, ASAN_POISON_FREED, abort);
}

#if __INTELLISENSE__
#	define __builtin_return_address(n) n
#	define __builtin_extract_return_addr(a) a
#endif
#define ASAN_LOAD_NOABORT(size)\
OBOS_NO_KASAN OBOS_EXPORT void __asan_load##size##_noabort(uintptr_t addr)\
{\
asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), false, false);\
}
#define ASAN_LOAD_ABORT(size)\
OBOS_NO_KASAN OBOS_EXPORT void __asan_load##size(uintptr_t addr)\
{\
asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), false, true);\
}
#define ASAN_STORE_NOABORT(size)\
OBOS_NO_KASAN OBOS_EXPORT void __asan_store##size##_noabort(uintptr_t addr)\
{\
asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), true, false);\
}
#define ASAN_STORE_ABORT(size)\
OBOS_NO_KASAN OBOS_EXPORT void __asan_store##size(uintptr_t addr)\
{\
asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), true, true);\
}
ASAN_LOAD_ABORT(1)
ASAN_LOAD_ABORT(2)
ASAN_LOAD_ABORT(4)
ASAN_LOAD_ABORT(8)
ASAN_LOAD_ABORT(16)
ASAN_LOAD_NOABORT(1)
ASAN_LOAD_NOABORT(2)
ASAN_LOAD_NOABORT(4)
ASAN_LOAD_NOABORT(8)
ASAN_LOAD_NOABORT(16)

ASAN_STORE_ABORT(1)
ASAN_STORE_ABORT(2)
ASAN_STORE_ABORT(4)
ASAN_STORE_ABORT(8)
ASAN_STORE_ABORT(16)
ASAN_STORE_NOABORT(1)
ASAN_STORE_NOABORT(2)
ASAN_STORE_NOABORT(4)
ASAN_STORE_NOABORT(8)
ASAN_STORE_NOABORT(16)

OBOS_NO_KASAN OBOS_EXPORT void __asan_load_n(uintptr_t addr, size_t size)
{
	asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), false, true);
}
OBOS_NO_KASAN OBOS_EXPORT void __asan_store_n(uintptr_t addr, size_t size)
{
	asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), true, true); 
}
OBOS_NO_KASAN OBOS_EXPORT void __asan_loadN_noabort(uintptr_t addr, size_t size)
{
	asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), false, false);
}
OBOS_NO_KASAN OBOS_EXPORT void __asan_storeN_noabort(uintptr_t addr, size_t size)
{
	asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), true, false);
}
OBOS_NO_KASAN OBOS_EXPORT void __asan_after_dynamic_init() { return; /* STUB */ }
OBOS_NO_KASAN OBOS_EXPORT void __asan_before_dynamic_init() { return; /* STUB */ }
OBOS_NO_KASAN OBOS_EXPORT void __asan_handle_no_return() { return; /* STUB */ }