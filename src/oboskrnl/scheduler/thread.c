/*
	oboskrnl/scheduler/thread.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <scheduler/thread_context_info.h>
#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/thread.h>

static uint64_t s_nextTID = 1;
cpu_local* Core_CpuInfo;
size_t Core_CpuCount;
obos_status CoreH_ThreadInitialize(thread* thr, thread_priority priority, thread_affinity affinity, const thread_ctx* ctx)
{
	if (!thr || !ctx || priority < 0 || priority >= THREAD_PRIORITY_MAX_VALUE || !affinity)
		return OBOS_STATUS_INVALID_ARGUMENT;
	thr->priority = priority;
	thr->status = THREAD_STATUS_READY;
	thr->tid = s_nextTID++;
	thr->context = *ctx;
	thr->affinity = affinity;
	thr->masterCPU = nullptr;
	thr->quantum = 0;
	return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_ThreadReady(thread* thr)
{
	// TODO: Implement.
	OBOS_UNUSED(thr);
	return OBOS_STATUS_UNIMPLEMENTED;
}
obos_status CoreH_ThreadReadyNode(thread* thr, thread_node* node)
{
	// Find the processor with the least threads using the current thread's priority.
	// Then, add the node to that list.
	if (!Core_CpuInfo)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!thr || !node)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (thr->priority < 0 || thr->priority > THREAD_PRIORITY_MAX_VALUE)
		return OBOS_STATUS_INVALID_ARGUMENT;
	cpu_local* cpuFound = nullptr;
	for (size_t cpui = 0; cpui < Core_CpuCount; cpui++)
	{
		cpu_local* cpu = &Core_CpuInfo[cpui];
		if (!(thr->affinity & CoreH_CPUIdToAffinity(cpu->id)))
			continue;
		if (!cpuFound || cpu->priorityLists[thr->priority].list.nNodes < cpuFound->priorityLists[thr->priority].list.nNodes)
			cpuFound = cpu;
	}
	if (!cpuFound)
		return OBOS_STATUS_INVALID_AFFINITY;
	node->data = thr;
	thr->masterCPU = cpuFound;
	thr->status = THREAD_STATUS_READY;
	thread_list* priorityList = &cpuFound->priorityLists[thr->priority].list;
	Core_ReadyThreadCount++;
	return CoreH_ThreadListAppend(priorityList, node);
}
obos_status CoreH_ThreadBlock(thread* thr, thread_node* node)
{
	if (!thr || !node)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if(!thr->masterCPU || thr->priority < 0 || thr->priority > THREAD_PRIORITY_MAX_VALUE)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (thr->status == THREAD_STATUS_BLOCKED)
		return OBOS_STATUS_SUCCESS;
	thr->status = THREAD_STATUS_BLOCKED;
	thr->quantum = 0;
	CoreH_ThreadListRemove(&thr->masterCPU->priorityLists[thr->priority].list, node);
	// TODO: Send an IPI of some sort to make sure the other CPU yields if this current thread is running.
	Core_ReadyThreadCount--;
	if (thr == Core_GetCurrentThread())
		Core_Yield();
	return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_ThreadListAppend(thread_list* list, thread_node* node)
{
	if (!list || !node)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!node->data)
		return OBOS_STATUS_INVALID_ARGUMENT;
	irql oldIrql = Core_SpinlockAcquire(&list->lock);
	if (list->tail)
		list->tail->next = node;
	if(!list->head)
		list->head = node;
	node->prev = list->tail;
	list->tail = node;
	list->nNodes++;
	node->data->snode = node;
	if (Core_SpinlockRelease(&list->lock, oldIrql) != OBOS_STATUS_SUCCESS)
	{
		Core_LowerIrql(oldIrql);
		Core_SpinlockForcedRelease(&list->lock);
	}
	return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_ThreadListRemove(thread_list* list, thread_node* node)
{
	if (!list || !node)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!node->data)
		return OBOS_STATUS_INVALID_ARGUMENT;
	irql oldIrql = Core_SpinlockAcquire(&list->lock);
	if (node->next)
		node->next->prev = node->prev;
	if (node->prev)
		node->prev->next = node->next;
	if (list->head == node)
		list->head = node->next;
	if (list->tail == node)
		list->tail = node->prev;
	node->data->snode = nullptr;
	list->nNodes--;
	if (Core_SpinlockRelease(&list->lock, oldIrql) != OBOS_STATUS_SUCCESS)
	{
		Core_LowerIrql(oldIrql);
		Core_SpinlockForcedRelease(&list->lock);
	}
	return OBOS_STATUS_SUCCESS;
}
uint32_t CoreH_CPUIdToAffinity(uint32_t cpuId)
{
	return (1 << cpuId);
}