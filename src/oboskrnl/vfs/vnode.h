/*
 * oboskrnl/vfs/vnode.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/pagecache.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <utils/list.h>

#include <locks/mutex.h>

enum 
{
    // This vnode has no type.
    VNODE_TYPE_NON,
    // This vnode represents a regular file.
    VNODE_TYPE_REG,
    // This vnode represents a directory.
    VNODE_TYPE_DIR,
    // This vnode represents a block device.
    VNODE_TYPE_BLK,
    // This vnode represents a character device.
    VNODE_TYPE_CHR,
    // This vnode represents a symbolic link.
    VNODE_TYPE_LNK,
    // This vnode represents a socket.
    VNODE_TYPE_SOCK,
    // This vnode represents a named pipe.
    VNODE_TYPE_FIFO,
    // This vnode represents a bad or dead file.
    VNODE_TYPE_BAD
};
enum 
{
    VFLAGS_MOUNTPOINT = 1,
    VFLAGS_IS_TTY = 2,
};

// basically a struct specinfo, but renamed.
typedef struct vdev
{
    dev_desc desc;
    driver_id* driver;
    void* data;
} vdev;
typedef struct file_perm
{
    bool other_exec : 1;
    bool other_write : 1;
    bool other_read : 1;
    bool group_exec : 1;
    bool group_write : 1;
    bool group_read : 1;
    bool owner_exec : 1;
    bool owner_write : 1;
    bool owner_read : 1;
} OBOS_PACK file_perm;
typedef struct vnode
{
    void* data;
    uint32_t vtype;
    uint32_t flags;
    struct mount* mount_point;
    union {
        struct mount* mounted;
        vdev*         device;
        // TODO: Add more stuff, such as pipes, sockets, etc.
    } un;
    size_t refs;
    file_perm perm;
    pagecache pagecache;
    size_t filesize; // filesize.
    uid owner_uid; // the owner's UID.
    gid group_uid; // the group's GID.
    dev_desc desc; // the cached device descriptor.
} vnode;
OBOS_EXPORT vnode* Drv_AllocateVNode(driver_id* drv, dev_desc desc, size_t filesize, vdev** dev, uint32_t type);