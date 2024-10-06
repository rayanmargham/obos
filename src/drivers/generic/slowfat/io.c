/*
 * drivers/generic/slowfat/io.c
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/limits.h>

#include <allocators/base.h>

#include <utils/list.h>
#include <utils/string.h>

#include <driver_interface/header.h>

#include <uacpi_libc.h>

#include <locks/mutex.h>

#include "structs.h"
#include "alloc.h"

static iterate_decision read_callback(uint32_t cluster, obos_status stat, void* udata_)
{
    uintptr_t* udata = udata_;
    uint8_t* buf = (void*)udata[0];
    size_t i = udata[1]++;
    size_t nClustersToRead = udata[2];
    int64_t bytesLeft = udata[3];
    size_t current_offset = udata[4];
    size_t cluster_offset = udata[5];
    fat_cache* cache = (void*)udata[6];
    obos_status* status = (void*)udata[7];
    uint8_t* cluster_buf = (void*)udata[8];
    const size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    if (stat != OBOS_STATUS_EOF && stat != OBOS_STATUS_SUCCESS)
    {
        *status = stat;
        return ITERATE_DECISION_STOP;
    }
    Vfs_FdSeek(cache->volume, ClusterToSector(cache, cluster)*cache->blkSize, SEEK_SET);
    *status = Vfs_FdRead(cache->volume, cluster_buf, bytesPerCluster, nullptr);
    if (obos_is_error(*status))
    {
        FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
        Core_MutexRelease(&cache->fd_lock);
        return ITERATE_DECISION_STOP;
    }
    memcpy(buf+current_offset, cluster_buf + cluster_offset, (size_t)bytesLeft <= bytesPerCluster ? (size_t)bytesLeft : bytesPerCluster);

    current_offset += bytesPerCluster;
    // cluster_offset += bytesPerCluster;
    cluster_offset = 0;
    bytesLeft -= bytesPerCluster;
    if (i >= nClustersToRead)
        return ITERATE_DECISION_STOP;
    if (bytesLeft < 0)
        return ITERATE_DECISION_STOP;
    return ITERATE_DECISION_CONTINUE;
}
obos_status read_sync(dev_desc desc, void* buf_, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    if (!desc || !buf_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    fat_cache* cache = cache_entry->owner;
    if (cache_entry->data.attribs & DIRECTORY)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (blkOffset >= cache_entry->data.filesize)
    {
        if (nBlkRead)
            *nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    size_t nToRead = blkCount;
    if ((blkOffset + blkCount) >= cache_entry->data.filesize)
        nToRead = cache_entry->data.filesize - blkOffset;
    const size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    size_t nClustersToRead = (nToRead / bytesPerCluster) + (((nToRead % bytesPerCluster) != 0) ? 1 : 0);
    uint32_t cluster = cache_entry->data.first_cluster_low;
    if (cache->fatType == FAT32_VOLUME)
        cluster |= ((uint32_t)cache_entry->data.first_cluster_high << 16);
    if (blkOffset)
        cluster += blkOffset/bytesPerCluster;
    uint8_t* cluster_buf = FATAllocator->ZeroAllocate(FATAllocator, 1, bytesPerCluster, nullptr);
    size_t current_offset = 0;
    size_t cluster_offset = blkOffset % bytesPerCluster;
    int64_t bytesLeft = blkCount;
    uint8_t *buf = buf_;
    Core_MutexAcquire(&cache->fd_lock);
    obos_status status = OBOS_STATUS_SUCCESS;
    uintptr_t udata[9] = {
        (uintptr_t)buf,
        (uintptr_t)0, /* i */
        (uintptr_t)nClustersToRead,
        (uintptr_t)bytesLeft,
        (uintptr_t)current_offset,
        (uintptr_t)cluster_offset,
        (uintptr_t)cache,
        (uintptr_t)&status,
        (uintptr_t)cluster_buf,
    };
    FollowClusterChain(cache, cluster, read_callback, udata);
    Core_MutexRelease(&cache->fd_lock);
    if (nBlkRead)
        *nBlkRead = (blkCount-(bytesLeft >= 0 ? bytesLeft : 0));
    FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
    return status;
}
obos_status write_sync(dev_desc desc, const void* buf_, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    if (!desc || !buf_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    fat_cache* cache = cache_entry->owner;
    if (cache_entry->data.attribs & DIRECTORY)
        return OBOS_STATUS_INVALID_ARGUMENT;
    bool requiresExpand = false;
    bool expandClusters = false;
    // TODO: Use block chain length.
    const size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    if ((blkOffset+blkCount) > cache_entry->data.filesize)
        requiresExpand = true;
    size_t filesz_cluster_aligned = cache_entry->data.filesize;
    if (filesz_cluster_aligned % bytesPerCluster)
        filesz_cluster_aligned += bytesPerCluster-(filesz_cluster_aligned%bytesPerCluster);
    if ((blkOffset+blkCount) >= filesz_cluster_aligned)
        requiresExpand = expandClusters = true;
    size_t nToWrite = blkCount;
    size_t nClustersToWrite = (nToWrite / bytesPerCluster) + (((nToWrite % bytesPerCluster) != 0) ? 1 : 0);
    obos_status status = OBOS_STATUS_SUCCESS;
    uint32_t cluster = cache_entry->data.first_cluster_low;
    if (cache->fatType == FAT32_VOLUME)
        cluster |= ((uint32_t)cache_entry->data.first_cluster_high << 16);
    if (requiresExpand)
    {
        uint32_t szClusters = ((cache_entry->data.filesize / bytesPerCluster) + ((cache_entry->data.filesize % bytesPerCluster) != 0));
        cache_entry->data.filesize += blkCount;
        uint32_t newSizeCls = ((cache_entry->data.filesize / bytesPerCluster) + ((cache_entry->data.filesize % bytesPerCluster) != 0));
        Core_MutexAcquire(&cache->fat_lock);
        uint8_t* cluster_buf = FATAllocator->ZeroAllocate(FATAllocator, 1, bytesPerCluster, nullptr);
        if (expandClusters)
            if (!ExtendClusters(cache, cluster, newSizeCls, szClusters))
            {
                uint32_t newCluster = AllocateClusters(cache, newSizeCls);
                if (newCluster == UINT32_MAX)
                {
                    Core_MutexRelease(&cache->fat_lock);
                    return OBOS_STATUS_NOT_ENOUGH_MEMORY;
                }
                Core_MutexAcquire(&cache->fd_lock);
                for (size_t i = 0; i < szClusters; i++)
                {
                    Vfs_FdSeek(cache->volume, ClusterToSector(cache, cluster+i)*cache->blkSize, SEEK_SET);
                    status = Vfs_FdRead(cache->volume, cluster_buf, bytesPerCluster, nullptr);
                    if (obos_is_error(status))
                    {
                        FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
                        Core_MutexRelease(&cache->fat_lock);
                        Core_MutexRelease(&cache->fd_lock);
                        return status;
                    }
                    Vfs_FdSeek(cache->volume, ClusterToSector(cache, newCluster+i)*cache->blkSize, SEEK_SET);
                    status = Vfs_FdWrite(cache->volume, cluster_buf, bytesPerCluster, nullptr);
                    if (obos_is_error(status))
                    {
                        FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
                        Core_MutexRelease(&cache->fat_lock);
                        Core_MutexRelease(&cache->fd_lock);
                        return status;
                    }
                }
                if (cluster)
                    FreeClusters(cache, cluster, szClusters);
                cluster = newCluster;
                cache_entry->data.first_cluster_high = cluster >> 16;
                cache_entry->data.first_cluster_low = cluster & 0xffff;
                Core_MutexRelease(&cache->fd_lock);
            }
        Core_MutexRelease(&cache->fat_lock);
        WriteFatDirent(cache, cache_entry, true);
    }
    uint8_t* cluster_buf = FATAllocator->ZeroAllocate(FATAllocator, 1, bytesPerCluster, nullptr);
    size_t current_offset = 0;
    size_t cluster_offset = blkOffset % bytesPerCluster;
    int64_t bytesLeft = blkCount;
    Core_MutexAcquire(&cache->fd_lock);
    uint32_t base_cluster = blkOffset / bytesPerCluster;
    Vfs_FdSeek(cache->volume, ClusterToSector(cache, cluster+base_cluster)*cache->blkSize, SEEK_SET);
    status = Vfs_FdRead(cache->volume, cluster_buf, bytesPerCluster, nullptr);
    if (obos_is_error(status))
    {
        FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
        Core_MutexRelease(&cache->fd_lock);
        return status;
    }
    const uint8_t *buf = buf_;
    for (size_t i = 0; i < nClustersToWrite && bytesLeft > 0; i++)
    {
        Vfs_FdSeek(cache->volume, ClusterToSector(cache, cluster+base_cluster+i)*cache->blkSize, SEEK_SET);
        memcpy(cluster_buf + cluster_offset, buf+current_offset, ((size_t)bytesLeft <= bytesPerCluster ? (size_t)bytesLeft : bytesPerCluster-cluster_offset));
        status = Vfs_FdWrite(cache->volume, cluster_buf, bytesPerCluster, nullptr);
        if (obos_is_error(status))
        {
            FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
            Core_MutexRelease(&cache->fd_lock);
            return status;
        }

        memzero(cluster_buf, bytesPerCluster);
        current_offset += bytesPerCluster;
        // cluster_offset += bytesPerCluster;
        cluster_offset = 0;
        bytesLeft -= bytesPerCluster;
    }
    Vfs_FdFlush(cache->volume);
    Core_MutexRelease(&cache->fd_lock);
    if (nBlkWritten)
        *nBlkWritten = blkCount;
    FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
    return OBOS_STATUS_SUCCESS;
}
obos_status trunc_file(dev_desc desc, size_t blkCount)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    fat_cache* cache = cache_entry->owner;
    if (cache_entry->data.filesize == blkCount)
        return OBOS_STATUS_SUCCESS;
    OBOS_ASSERT(cache_entry->data.filesize >= blkCount);
    if (blkCount > cache_entry->data.filesize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    const size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    uint32_t szClusters = ((cache_entry->data.filesize / bytesPerCluster) + ((cache_entry->data.filesize % bytesPerCluster) != 0));
    cache_entry->data.filesize = blkCount;
    uint32_t newSizeCls = ((cache_entry->data.filesize / bytesPerCluster) + ((cache_entry->data.filesize % bytesPerCluster) != 0));
    uint32_t cluster = cache_entry->data.first_cluster_low;
    if (cache->fatType == FAT32_VOLUME)
        cluster |= ((uint32_t)cache_entry->data.first_cluster_high << 16);
    Core_MutexAcquire(&cache->fat_lock);
    TruncateClusters(cache, cluster, newSizeCls, szClusters);
    if (!newSizeCls)
    {
        FreeClusters(cache, cluster, szClusters);
        cache_entry->data.first_cluster_low = 0;
        cache_entry->data.first_cluster_high = 0;
    }
    Core_MutexRelease(&cache->fat_lock);
    WriteFatDirent(cache, cache_entry, true);
    Vfs_FdFlush(cache->volume);
    return OBOS_STATUS_SUCCESS;
}
obos_status WriteFatDirent(fat_cache* cache, fat_dirent_cache* cache_entry, bool lock)
{
    const size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    if (lock)
        Core_MutexAcquire(&cache->fd_lock);
    Vfs_FdSeek(cache->volume, cache_entry->dirent_fileoff, SEEK_SET);
    uint8_t* cluster_buf = FATAllocator->ZeroAllocate(FATAllocator, 1, bytesPerCluster, nullptr);
    obos_status status = Vfs_FdRead(cache->volume, cluster_buf, bytesPerCluster, nullptr);
    if (obos_is_error(status))
    {
        FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
        if (lock)
            Core_MutexRelease(&cache->fd_lock);
        return status;
    }
    memcpy(cluster_buf+cache_entry->dirent_offset, &cache_entry->data, sizeof(cache_entry->data));
    Vfs_FdSeek(cache->volume, cache_entry->dirent_fileoff, SEEK_SET);
    status = Vfs_FdWrite(cache->volume, cluster_buf, bytesPerCluster, nullptr);
    if (obos_is_error(status))
    {
        FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
        if (lock)
            Core_MutexRelease(&cache->fd_lock);
        return status;
    }
    if (lock)
        Core_MutexRelease(&cache->fd_lock);
    FATAllocator->Free(FATAllocator, cluster_buf, bytesPerCluster);
    Vfs_FdFlush(cache->volume);
    return OBOS_STATUS_SUCCESS;
}