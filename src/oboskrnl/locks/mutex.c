/*
 * oboskrnl/locks/mutex.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <irq/irql.h>

#include <locks/mutex.h>
#include <locks/spinlock.h>
#include <locks/wait.h>

#include <stdatomic.h>

#ifdef __x86_64__
#	define spinlock_hint() __builtin_ia32_pause()
#elif defined(__m68k__)
#	define spinlock_hint() asm("nop")
#endif

obos_status Core_MutexAcquire(mutex* mut)
{
    if (!mut)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (mut->who == Core_GetCurrentThread())
        return OBOS_STATUS_RECURSIVE_LOCK;
    // Spin for a bit.
    irql oldIrql = Core_GetIrql() < IRQL_DISPATCH ? Core_RaiseIrql(IRQL_DISPATCH) : IRQL_INVALID;
    int spin = 100000;
    bool success = true;
    while (atomic_flag_test_and_set_explicit(&mut->lock, memory_order_seq_cst) && success)
    {
        spinlock_hint();
        if (spin-- >= 0)
            success = false;
    }
    if (oldIrql != IRQL_INVALID)
        Core_LowerIrql(oldIrql);
    if (success)
    {
        mut->who = Core_GetCurrentThread();
        mut->locked = true;
        return OBOS_STATUS_SUCCESS;
    }
    Core_WaitOnObject(&mut->hdr);
    while (atomic_flag_test_and_set_explicit(&mut->lock, memory_order_seq_cst) && success)
        spinlock_hint();
    mut->who = Core_GetCurrentThread();
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_MutexTryAcquire(mutex* mut)
{
    if (!mut)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (mut->locked)
        return OBOS_STATUS_IN_USE;
    return Core_MutexAcquire(mut);
}
obos_status Core_MutexRelease(mutex* mut)
{
    if (!mut)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!mut->locked)
        return OBOS_STATUS_SUCCESS;
    if (mut->who != Core_GetCurrentThread())
        return OBOS_STATUS_ACCESS_DENIED;
    atomic_flag_clear_explicit(&mut->lock, memory_order_seq_cst);
    obos_status status = CoreH_SignalWaitingThreads(&mut->hdr, false, false);
    if (obos_is_error(status))
        return status;
    return OBOS_STATUS_SUCCESS;
}
bool Core_MutexAcquired(mutex* mut)
{
    if (!mut)
        return false;
    return mut->locked;
}