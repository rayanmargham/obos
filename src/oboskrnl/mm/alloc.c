/*
 * oboskrnl/mm/alloc.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/
#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/page.h>
#include <mm/bare_map.h>
#include <mm/swap.h>
#include <mm/pmm.h>

#include <scheduler/process.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/pagecache.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

allocator_info* OBOS_NonPagedPoolAllocator;
allocator_info* Mm_Allocator;

#define set_statusp(status, to) (status) ? *(status) = (to) : (void)0
void* MmH_FindAvailableAddress(context* ctx, size_t size, vma_flags flags, obos_status* status)
{
    if (!ctx)
    {
        set_statusp(status, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    size_t pgSize = flags & VMA_FLAGS_HUGE_PAGE ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    if (size % pgSize)
        size -= (size % pgSize);
    uintptr_t base = 
        ctx->owner->pid == 1 ?
        OBOS_KERNEL_ADDRESS_SPACE_BASE :
        OBOS_USER_ADDRESS_SPACE_BASE,
              limit = 
        ctx->owner->pid == 1 ?
        OBOS_KERNEL_ADDRESS_SPACE_LIMIT :
        OBOS_USER_ADDRESS_SPACE_LIMIT;
#if OBOS_ARCHITECTURE_BITS != 32
    if (flags & VMA_FLAGS_32BIT)
    {
        base = 0x1000;
        limit = 0xfffff000;
    }
#endif
	page_range* currentNode = nullptr;
    page_range what = {.virt=base};
	page_range* lastNode = RB_FIND(page_tree, &ctx->pages, &what);
	uintptr_t lastAddress = base;
	uintptr_t found = 0;
	for (currentNode = RB_MIN(page_tree, &ctx->pages); 
        currentNode; 
        currentNode = RB_NEXT(page_tree, &ctx->pages, currentNode))
    {
		uintptr_t currentNodeAddr = currentNode->virt;
		if (currentNodeAddr < base)
		    continue;
        if (currentNodeAddr >= limit)
            break; // Because of the properties of an RB-Tree, we can break here.
		if ((currentNodeAddr - lastAddress) >= (size + pgSize))
		{
            if (!lastNode)
                continue;
            found = lastAddress;
            break;
		}
		lastAddress = currentNodeAddr + currentNode->size;
        lastNode = currentNode;
	}
    if (!found)
	{
		page_range* currentNode = lastNode;
		if (currentNode)
			found = (currentNode->virt + currentNode->size);
		else
			found = base;
	}
	if (!found)
	{
		if (status)
			*status = OBOS_STATUS_NOT_ENOUGH_MEMORY;
		return nullptr;
	}
    // OBOS_Debug("%s %p\n", __func__, found);
    return (void*)found;
}
page* Mm_AnonPage = nullptr;
void* Mm_VirtualMemoryAlloc(context* ctx, void* base_, size_t size, prot_flags prot, vma_flags flags, fd* file, obos_status* ustatus)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    set_statusp(ustatus, status);
    if (!ctx || !size)
    {
        set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    if (flags & VMA_FLAGS_RESERVE)
        file = nullptr;
    if (file && flags & VMA_FLAGS_NON_PAGED)
    {
        set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    if (file && !file->vn)
    {
        set_statusp(ustatus, OBOS_STATUS_UNINITIALIZED);
        return nullptr;
    }
    if (file)
        flags &= ~VMA_FLAGS_HUGE_PAGE; // you see, page caches don't really use huge pages, so we have to force huge pages off.
    uintptr_t base = (uintptr_t)base_;
    const size_t pgSize = (flags & VMA_FLAGS_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    if (base % pgSize)
    {
        set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    if (OBOS_HUGE_PAGE_SIZE == OBOS_PAGE_SIZE)
        flags &= ~VMA_FLAGS_HUGE_PAGE;
    if (flags & VMA_FLAGS_32BITPHYS)
        file = nullptr;
    size_t filesize = 0;
    if (file)
    {
        if (file->vn->vtype != VNODE_TYPE_REG && file->vn->vtype != VNODE_TYPE_BLK /* hopefully doesn't break */)
        {
            set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
            return nullptr;
        }
        if (file->vn->filesize < size)
            size = file->vn->filesize; // Truncated.
        if ((file->offset+size > file->vn->filesize))
            size = (file->offset+size) - file->vn->filesize;
        filesize = size;
        if (size % pgSize)
            size += (pgSize-(size%pgSize));
        if (!(file->flags & FD_FLAGS_READ))
        {
            // No.
            set_statusp(ustatus, OBOS_STATUS_ACCESS_DENIED);
            return nullptr;
        }
        if (!(file->flags & FD_FLAGS_WRITE) && !(flags & VMA_FLAGS_PRIVATE))
            prot |= OBOS_PROTECTION_READ_ONLY;
    }
    if (size % pgSize)
        size += (pgSize-(size%pgSize));
    if (flags & VMA_FLAGS_GUARD_PAGE)
        size += pgSize;
    if ((flags & VMA_FLAGS_PREFAULT || flags & VMA_FLAGS_PRIVATE) && file)
        VfsH_PageCacheGetEntry(&file->vn->pagecache, file->vn, file->offset, size, nullptr);
    irql oldIrql = Core_SpinlockAcquireExplicit(&ctx->lock, IRQL_DISPATCH, true);
    top:
    if (!base)
    {
        base = (uintptr_t)MmH_FindAvailableAddress(ctx, size, flags & ~VMA_FLAGS_GUARD_PAGE, &status);
        if (obos_is_error(status))
        {
            set_statusp(ustatus, status);
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return nullptr;
        }
    }
    // We shouldn't reallocate the page(s).
    // Check if they exist so we don't do that by accident.
    // page what = {};
    // bool exists = false;
    // for (uintptr_t addr = base; addr < base + size; addr += pgSize)
    // {
    //     what.addr = addr;
    //     page* found = RB_FIND(page_tree, &ctx->pages, &what);
    //     if (found && !found->reserved)
    //     {
    //         exists = true;
    //         break;
    //     }
    // }
    page_range what = {.virt=base,.size=size};
    page_range* rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (rng && !rng->reserved)
    {
        if (flags & VMA_FLAGS_HINT)
        {
            base = 0;
            goto top;
        }
        else
        {
            set_statusp(ustatus, OBOS_STATUS_IN_USE);
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return nullptr;
        }
    }
    // TODO: Optimize by splitting really big allocations (> OBOS_HUGE_PAGE_SIZE) into huge pages and normal pages.
    off_t currFileOff = file ? file->offset : 0;
    pagecache_mapped_region* reg = file ?
        Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(pagecache_mapped_region), nullptr) : nullptr;
    page_range* pc_range = nullptr;
    if (file)
    {
        page_range what = {.virt=(uintptr_t)file->vn->pagecache.data};
        pc_range = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
        reg->fileoff = file->offset;
        reg->sz = filesize;
        reg->addr = base;
        reg->owner = &file->vn->pagecache;
        reg->ctx = ctx;
        LIST_APPEND(mapped_region_list, &reg->owner->mapped_regions, reg);
    }
    bool present = false;
    volatile bool isNew = true;
    if (!rng)
    {
        rng = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
        present = rng->prot.present = !(flags & VMA_FLAGS_RESERVE);
        rng->prot.huge_page = flags & VMA_FLAGS_HUGE_PAGE;
        if (!(flags & VMA_FLAGS_PRIVATE) || !file)
        {
            rng->prot.rw = !(prot & OBOS_PROTECTION_READ_ONLY);
            rng->pageable = !(flags & VMA_FLAGS_NON_PAGED);
        }
        if (!(flags & VMA_FLAGS_PRIVATE) && file)
            rng->prot.rw = false; // force it off so that we can mark dirty pages.
        rng->prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
        rng->prot.user = prot & OBOS_PROTECTION_USER_PAGE;
        rng->prot.ro = prot & OBOS_PROTECTION_READ_ONLY;
        rng->prot.uc = prot & OBOS_PROTECTION_CACHE_DISABLE;
        rng->hasGuardPage = (flags & VMA_FLAGS_GUARD_PAGE);
        rng->mapped_here = reg;
        rng->size = size;
        rng->virt = base;
        // note for future me: the '~' is intentionally there
        rng->pageable = ~flags & VMA_FLAGS_NON_PAGED;
        rng->reserved = (flags & VMA_FLAGS_RESERVE);
        // this can be implied by doing '!rng->cow && rng->mapped_here'
        // rng->un.shared = reg ? ~flags & VMA_FLAGS_PRIVATE : false;
        rng->phys32 = (flags & VMA_FLAGS_32BITPHYS);
        rng->ctx = ctx;
    }
    else 
    {
        isNew = false;
        // OBOS_UNUSED(isNew);
        rng->size_committed += size;
        if (rng->size_committed >= rng->size)
            rng->reserved = false;
        present = true;
    }
    page* phys = 0;
    if (!file && !(flags & VMA_FLAGS_NON_PAGED) && !(flags & VMA_FLAGS_RESERVE))
    {
        OBOS_ASSERT(Mm_AnonPage);
        // Use anon physical page.
        phys = Mm_AnonPage;
    }
    for (uintptr_t addr = base; addr < (base+size); addr += pgSize)
    {
        bool isPresent = !(rng->hasGuardPage && (base==addr)) && present;
        bool cow = false;
        // for (volatile bool b = (addr==0xffffff00003d3000); b; )
        //     ;
        if (isPresent)
        {
            if (!file && (flags & VMA_FLAGS_NON_PAGED))
                phys = MmH_PgAllocatePhysical(flags & VMA_FLAGS_32BITPHYS, flags & VMA_FLAGS_HUGE_PAGE);
            else if (file)
            {
                // File page.
                page_info info = {};
                MmS_QueryPageInfo(ctx->pt, (uintptr_t)reg->owner->data+currFileOff, &info, nullptr);
                if (flags & VMA_FLAGS_PRIVATE)
                {
                    // Private.
                    // Moooo (CoW)
                    cow = true;
                }
                isPresent = info.prot.present;
                page what = {.phys=info.phys};
                phys = isPresent ? RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what) : nullptr;
                if (phys)
                    MmH_RefPage(phys);
                rng->cow = cow;
                if (cow)
                {
                    rng->un.cow_type = COW_SYMMETRIC;
                    pc_range->cow = true;
                    pc_range->un.cow_type = COW_SYMMETRIC;
                    info.prot.rw = false;
                    MmS_SetPageMapping(ctx->pt, &info, info.phys, false);
                }
            }
            else
            {
                rng->un.cow_type = COW_ASYMMETRIC;
                rng->cow = true;
                MmH_RefPage(phys);
            }
        }
        // Append the virtual page to phys on demand as apposed to now to save memory.
        // An example of where it'd be added is on swap out, as the virtual_pages list is not needed until then.
        page_info curr = {};
        curr.range = rng;
        curr.virt = addr;
        curr.phys = phys ? phys->phys : 0;
        curr.prot = rng->prot;
        curr.prot.rw = cow ? false : rng->prot.rw;
        curr.prot.present = isPresent;
        if (rng->cow && rng->un.cow_type == COW_ASYMMETRIC)
        curr.prot.present = false;
        OBOS_ASSERT(phys != 0 || !isPresent);
        MmS_SetPageMapping(ctx->pt, &curr, curr.phys, false);
        // if (rng->pageable && !rng->reserved && isPresent && !file)
        //     Mm_SwapOut(&curr);
        currFileOff += pgSize;
    }
    if (!(flags & VMA_FLAGS_RESERVE))
    {
        if (flags & VMA_FLAGS_GUARD_PAGE)
            size -= pgSize;
        if (!(flags & VMA_FLAGS_NON_PAGED))
            ctx->stat.pageable += size;
        else
            ctx->stat.nonPaged += size;
        if (!isNew)
            ctx->stat.reserved -= size;
        else
            ctx->stat.committedMemory += size;
    }
    else
        ctx->stat.reserved += size;
    OBOS_ASSERT(rng->size);
    RB_INSERT(page_tree, &ctx->pages, rng);
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    if (flags & VMA_FLAGS_GUARD_PAGE)
        base += pgSize;
    // printf("mapped %d bytes at %p\n", size, base);
    return (void*)base;
}
obos_status Mm_VirtualMemoryFree(context* ctx, void* base_, size_t size)
{
    // OBOS_Debug("%s %p\n", __func__, base_);
    uintptr_t base = (uintptr_t)base_;
    // if (base % OBOS_PAGE_SIZE)
    //     return OBOS_STATUS_INVALID_ARGUMENT;
    base -= (base%OBOS_PAGE_SIZE);
    if (!ctx || !base || !size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (size % OBOS_PAGE_SIZE)
        size += (OBOS_PAGE_SIZE-(size%OBOS_PAGE_SIZE));
    /* We need to:
        - Unmap the pages
        - Remove the pages from any VMM data structures (working set, page tree, referenced list)
    */
    
    // Verify the pages' existence.
    page_range what = {.virt=base,.size=size};
    page_range* rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (!rng)
        return OBOS_STATUS_NOT_FOUND;
    // printf("freeing %d at %p. called from %p\n", size, base, __builtin_return_address(0));
    irql oldIrql = Core_SpinlockAcquireExplicit(&ctx->lock, IRQL_DISPATCH, true);
    bool sizeHasGuardPage = false;
    if (rng->hasGuardPage)
    {
        const size_t pgSize = (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        base -= pgSize;
        if (size == (rng->size - pgSize))
        {
            size += pgSize;
            sizeHasGuardPage = true;
        }
    }
    bool full = true;
    page_protection new_prot = rng->prot;
    new_prot.present = false;
    if (rng->virt != base || rng->size != size)
    {
        // OBOS_Debug("untested code path\n");
        full = false;
        // Split.
        // If rng->virt == base, or rng->size == size
        // Split at rng->virt+size (size: rng->size-size) and remove the other range
        // If rng->size != size, and rng->virt != base
        // Split at base (size: ((base-rng->virt)+rng->size-size)-rng->virt)
        if (rng->virt != base && rng->size != size)
        {
            if ((base + size) >= (rng->virt+rng->size))
                return OBOS_STATUS_INVALID_ARGUMENT;
            // We need two ranges, one for the range behind base, and another for the range after.
            page_range* before = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            page_range* after = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            memcpy(before, rng, sizeof(*before));
            memcpy(after, rng, sizeof(*before));
            before->size = base-before->virt;
            after->virt = before->virt+before->size+size;
            after->size = (after->virt-before->virt);
            after->hasGuardPage = false;
            memzero(&before->working_set_nodes, sizeof(before->working_set_nodes));
            memzero(&after->working_set_nodes, sizeof(after->working_set_nodes));
            // printf("split %p-%p into %p-%p and %p-%p\n", base, size+base, before->virt, before->virt+before->size, after->virt, after->virt+after->size);
            for (working_set_node* curr = rng->working_set_nodes.head; curr; )
            {
                working_set_node* next = curr->next;
                if (curr->data->info.virt >= before->virt && curr->data->info.virt < after->virt)
                {
                    curr->data->free = true; // mark for deletion.
                    Mm_Allocator->Free(Mm_Allocator, curr, sizeof(*curr));
                    curr = next;
                    continue;
                }
                if (curr->data->info.virt < before->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    curr->data->info.range = before;
                    APPEND_WORKINGSET_PAGE_NODE(before->working_set_nodes, &curr->data->pr_node);
                }
                if (curr->data->info.virt >= after->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    curr->data->info.range = after;
                    APPEND_WORKINGSET_PAGE_NODE(after->working_set_nodes, &curr->data->pr_node);
                }
                curr = next;
            }
            RB_REMOVE(page_tree, &ctx->pages, rng);
            RB_INSERT(page_tree, &ctx->pages, before);
            RB_INSERT(page_tree, &ctx->pages, after);
            rng->ctx = nullptr;
            Mm_Allocator->Free(Mm_Allocator, rng, sizeof(*rng));
            rng = nullptr;
        }
        else if (rng->virt == base || rng->size == size)
        {
            rng->size = (rng->size-size);
            rng->virt += size;
            // printf("split %p-%p into %p-%p\n", base, size+base, rng->virt, rng->virt+rng->size);
            for (working_set_node* curr = rng->working_set_nodes.head; curr; )
            {
                working_set_node* next = curr->next;
                if (curr->data->info.virt < rng->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    curr->data->free = true;
                    Mm_Allocator->Free(Mm_Allocator, curr, sizeof(*curr));
                }
                curr = next;
            }
        }
    }
    OBOS_ASSERT(rng->size);
    
    page_info pg = {};
    pg.prot = new_prot;
    pg.range = nullptr;
    for (uintptr_t addr = base; addr < (base+size); addr += new_prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)
    {
        pg.virt = addr;
        page_info info = {};
        MmS_QueryPageInfo(ctx->pt, addr, &info, nullptr);
        info.range = rng;
        if (!info.prot.present && !rng->mapped_here && rng->pageable)
        {
            if (obos_is_success(Mm_SwapIn(&info, nullptr)))
                ctx->stat.paged -= (info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        }
        if (!info.prot.is_swap_phys && info.phys)
        {
            page what = {.phys=info.phys};
            page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
            // printf("derefing physical page %p representing %p\n", pg->phys, addr);
            MmH_DerefPage(pg);
        }
        // for(volatile bool b = (addr == 0xffffff0000039000); b; );
        // printf("unmapping %p\n", addr);
        MmS_SetPageMapping(ctx->pt, &pg, 0, true);
    }
    if (rng)
    {
        if (sizeHasGuardPage)
            size -= (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        if (rng->reserved)
            ctx->stat.reserved -= size;
        else
            ctx->stat.committedMemory -= size;
        if (rng->pageable)
            ctx->stat.pageable -= size;
        else
            ctx->stat.nonPaged -= size;
    }
    if (full)
    {
        for (working_set_node* curr = rng->working_set_nodes.head; curr; )
        {
            working_set_node* next = curr->next;
            REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
            curr->data->free = true;
            Mm_Allocator->Free(Mm_Allocator, curr, sizeof(*curr));
            curr = next;
        }
        RB_REMOVE(page_tree, &ctx->pages, rng);
        Mm_Allocator->Free(Mm_Allocator, rng, sizeof(*rng));
    }
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
obos_status Mm_VirtualMemoryProtect(context* ctx, void* base_, size_t size, prot_flags prot, int isPageable)
{
    uintptr_t base = (uintptr_t)base_;
    if (base % OBOS_PAGE_SIZE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!ctx || !base || !size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (size % OBOS_PAGE_SIZE)
        size += (OBOS_PAGE_SIZE-(size%OBOS_PAGE_SIZE));
    if (prot == OBOS_PROTECTION_SAME_AS_BEFORE && isPageable > 1)
        return OBOS_STATUS_SUCCESS;
    // Verify the pages' existence.
    page_range what = {.virt=base,.size=size};
    page_range* rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (!rng)
        return OBOS_STATUS_NOT_FOUND;
    irql oldIrql = Core_SpinlockAcquireExplicit(&ctx->lock, IRQL_DISPATCH, true);
    page_protection new_prot = rng->prot;
    if (~prot & OBOS_PROTECTION_SAME_AS_BEFORE)
    {
        new_prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
        new_prot.user = prot & OBOS_PROTECTION_USER_PAGE;
        new_prot.ro = prot & OBOS_PROTECTION_READ_ONLY;
        new_prot.rw = !new_prot.ro;
        new_prot.uc = prot & OBOS_PROTECTION_CACHE_DISABLE;
    }
    else
    {
        if (prot & OBOS_PROTECTION_EXECUTABLE)
            new_prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
        if (prot & OBOS_PROTECTION_USER_PAGE)
            new_prot.user = prot & OBOS_PROTECTION_USER_PAGE;
        if (prot & OBOS_PROTECTION_READ_ONLY)
            new_prot.ro = prot & OBOS_PROTECTION_READ_ONLY;
        if (prot & OBOS_PROTECTION_CACHE_DISABLE)
            new_prot.uc = prot & OBOS_PROTECTION_CACHE_DISABLE;
        if (prot & OBOS_PROTECTION_CACHE_ENABLE)
            new_prot.uc = !(prot & OBOS_PROTECTION_CACHE_ENABLE);
    }
    bool pageable = isPageable > 1 ? rng->pageable : (bool)isPageable;
    if (rng->virt != base || rng->size != size)
    {
        // Split.
        // If rng->virt == base, or rng->size == size
        // Split at rng->virt+size (size: rng->size-size) and remove the other range
        // If rng->size != size, and rng->virt != base
        // Split at base (size: ((base-rng->virt)+rng->size-size)-rng->virt)
        if (rng->virt != base && rng->size != size)
        {
            // OBOS_Debug("untested code path 1\n");
            if ((base + size) >= (rng->virt+rng->size))
                return OBOS_STATUS_INVALID_ARGUMENT;
            // We need three ranges, one for the range behind base, another for the range after, and another for the new protection flags.
            page_range* before = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            page_range* after = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            page_range* new = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            memcpy(new, rng, sizeof(*rng));
            memcpy(before, rng, sizeof(*rng));
            memcpy(after, rng, sizeof(*rng));
            before->size = base-before->virt;
            after->virt = before->virt+before->size+size;
            after->size = rng->size-(after->virt-rng->virt);
            memzero(&before->working_set_nodes, sizeof(before->working_set_nodes));
            memzero(&after->working_set_nodes, sizeof(after->working_set_nodes));
            new->prot = new_prot;
            new->pageable = pageable;
            for (working_set_node* curr = rng->working_set_nodes.head; curr; )
            {
                working_set_node* next = curr->next;
                if (curr->data->info.virt >= before->virt && curr->data->info.virt < after->virt)
                {
                    curr->data->info.range = new;
                    if (!pageable)
                        curr->data->free = true;
                    curr = next;
                    continue;
                }
                if (curr->data->info.virt < before->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    APPEND_WORKINGSET_PAGE_NODE(before->working_set_nodes, &curr->data->pr_node);
                    curr->data->info.range = before;
                }
                if (curr->data->info.virt >= after->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    APPEND_WORKINGSET_PAGE_NODE(after->working_set_nodes, &curr->data->pr_node);
                    curr->data->info.range = after;
                }
                curr = next;
            }
            new->prot = new_prot;
            new->pageable = pageable;
            new->virt = base;
            new->size = size;
            // printf(
            //     ""
            //     "0x%p-0x%p\n"
            //     "0x%p-0x%p\n"
            //     "0x%p-0x%p\n"
            //     "0x%p-0x%p\n",
            //     before->virt, before->virt+before->size,
            //     new->virt, new->virt+new->size,
            //     after->virt, after->virt+after->size,
            //     rng->virt, rng->virt+rng->size
            // );
            RB_REMOVE(page_tree, &ctx->pages, rng);
            RB_INSERT(page_tree, &ctx->pages, before);
            RB_INSERT(page_tree, &ctx->pages, after);
            RB_INSERT(page_tree, &ctx->pages, new);
            Mm_Allocator->Free(Mm_Allocator, rng, sizeof(*rng));
            rng = new;
        }
        else if (rng->virt == base || rng->size == size)
        {
            // OBOS_Debug("untested code path 2\n");
            // printf(
            //     ""
            //     "0x%p-0x%p\n",
            //     rng->virt, rng->virt+rng->size
            // );
            page_range* new = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            memcpy(new, rng, sizeof(*rng));
            new->size = size;
            rng->size = (rng->size-size);
            if (base > rng->virt)
                rng->virt = base;
            else
                rng->virt += size;
            // printf(
            //     ""
            //     "0x%p-0x%p\n"
            //     "0x%p-0x%p\n",
            //     rng->virt, rng->virt+rng->size,
            //     new->virt, new->virt+new->size
            // );
            new->prot = new_prot;
            new->pageable = pageable;
            RB_INSERT(page_tree, &ctx->pages, new);
            size_t szUpdated = 0;
            for (working_set_node* curr = rng->working_set_nodes.head; curr && szUpdated < size; )
            {
                working_set_node* next = curr->next;
                if (curr->data->info.virt >= new->virt && curr->data->info.virt < rng->virt)
                {
                    szUpdated += (curr->data->info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
                    curr->data->info.range = new;
                    if (!pageable)
                        curr->data->free = true;
                }
                curr = next;
            }
            rng = new;
        }
    }
    if (rng)
        OBOS_ASSERT(rng->size);
    page_info pg = {};
    pg.prot = new_prot;
    pg.range = nullptr;
    for (uintptr_t addr = base; addr < (base+size); addr += new_prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)
    {
        pg.virt = addr;
        // printf("0x%p %08x\n", pg.virt, prot);
        page_info info = {};
        MmS_QueryPageInfo(ctx->pt, addr, &info, nullptr);
        pg.prot.present = info.prot.present;
        MmS_SetPageMapping(ctx->pt, &pg, info.phys, false);
    }
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}