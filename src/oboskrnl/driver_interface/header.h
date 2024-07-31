/*
 * oboskrnl/driver_interface/header.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

enum { OBOS_DRIVER_MAGIC = 0x00116d868ac84e59 };
// Not required, but can speed up loading times if the driver header is put in here.
#define OBOS_DRIVER_HEADER_SECTION ".driverheader"

typedef enum driver_header_flags
{
    /// <summary>
    /// Should the driver be detected through ACPI?
    /// </summary>
    DRIVER_HEADER_FLAGS_DETECT_VIA_ACPI = 0x1,
    /// <summary>
    /// Should the driver be detected through PCI?
    /// </summary>
    DRIVER_HEADER_FLAGS_DETECT_VIA_PCI = 0x2,
    /// <summary>
    /// If the driver does not have an entry point, specify this flag.
    /// </summary>
    DRIVER_HEADER_FLAGS_NO_ENTRY = 0x4,
    /// <summary>
    /// If set, the driver chooses its entry point's stack size.
    /// Ignored if DRIVER_HEADER_FLAGS_NO_ENTRY is set.
    /// </summary>
    DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE = 0x8,
    /// <summary>
    /// Whether the driver exposes the standard driver interfaces to the kernel via the function table in the driver header.
    /// <para/>If unset, the driver needs to expose its own interfaces using DRV_EXPORT.
    /// <para/>NOTE: Every driver needs to have an ioctl callback in the function table, despite the state of this flag.
    /// </summary>
    DRIVER_HEADER_HAS_STANDARD_INTERFACES = 0x10,
    /// <summary>
    /// This flag should be set if the device is read from pipe-style.<para/>
    /// If this flag is set, any blkOffset parameter should be ignored. 
    /// </summary>
    DRIVER_HEADER_PIPE_STYLE_DEVICE = 0x20,
} driver_header_flags;
typedef enum iterate_decision
{
    ITERATE_DECISION_CONTINUE,
    ITERATE_DECISION_STOP,
} iterate_decision;
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
typedef enum file_type
{
    FILE_TYPE_DIRECTORY,
    FILE_TYPE_REGULAR_FILE,
    FILE_TYPE_SYMBOLIC_LINK,
} file_type;
// Represents a driver-specific object.
// This could be a disk, partition, file, etc.
// This must be unique per-driver.
typedef int dev_desc;
typedef struct driver_ftable
{
    // Note: If there is not an OBOS_STATUS for an error that a driver needs to return, rather choose the error closest to the error that you want to report,
    // or return OBOS_STATUS_INTERNAL_ERROR.

    // ---------------------------------------
    // ------- START GENERIC FUNCTIONS -------
    
    // NOTE: Every driver should have these functions implemented.

    obos_status(*get_blk_size)(dev_desc desc, size_t* blkSize);
    obos_status(*get_max_blk_count)(dev_desc desc, size_t* count);
    obos_status(*read_sync)(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset);
    obos_status(*write_sync)(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset);
    obos_status(*foreach_device)(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount));
    obos_status(*query_user_readable_name)(dev_desc what, const char** name); // unrequired for fs drivers.
    // The driver dictates what the request means, and what its parameters are.
    obos_status(*ioctl)(size_t nParameters, uint64_t request, ...);
    // Called on driver unload.
    // Frees all the driver's allocated resources, as the kernel 
    // does not keep a track of allocated resources, and cannot free them on driver unload, causing a
    // memory leak.
    void(*driver_cleanup_callback)();

    // -------- END GENERIC FUNCTIONS --------
    // ---------------------------------------
    // ---------- START FS FUNCTIONS ---------

    // NOTE: FS Drivers must always return one from get_blk_size
    // get_max_blk_count is the equivalent to get_filesize
    // NOTE: Every function here must be pointing to something if the driver is an fs driver, otherwise they must be pointing to nullptr.

    // lifetime of *path is dicated by the driver.
    obos_status(*query_path)(dev_desc desc, const char** path);
    obos_status(*path_search)(dev_desc* found, const char* what);
    obos_status(*move_desc_to)(dev_desc desc, const char* where);
    obos_status(*mk_file)(dev_desc* newDesc, dev_desc parent, const char* name, file_type type);
    obos_status(*remove_file)(dev_desc desc);
    obos_status(*get_file_perms)(dev_desc desc, file_perm *perm);
    obos_status(*set_file_perms)(dev_desc desc, file_perm newperm);
    obos_status(*get_file_type)(dev_desc desc, file_type *type);
    obos_status(*list_dir)(dev_desc dir, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount));
    // ----------- END FS FUNCTIONS ----------
    // ---------------------------------------
} driver_ftable;
typedef struct driver_header
{
    uint64_t magic;
    uint32_t flags;
    struct
    {
        uint32_t classCode;
        /// <summary>
        /// If a bit is set, the bit number will be the value.
        /// <para></para>
        /// This bitfield can have more than bit set (for multiple values).
        /// </summary>
        __uint128_t subclass;
        /// <summary>
        /// If a bit is set, the bit number will be the value.
        /// <para></para>
        /// This bitfield can have more than bit set (for multiple values).
        /// <para></para>
        /// If no bit is set any prog if is assumed.
        /// </summary>
        __uint128_t progIf;
    } pciId;
    struct
    {
        // These strings are not null-terminated.
        // The PnP IDs for the driver.
        // Each one of these is first compared with the HID.
        // Then, each one of these is compared with the CID.
        char pnpIds[32][8];
        // Ranges from 1-32 inclusive.
        size_t nPnpIds;
    } acpiId;
    size_t stackSize; // If DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE is set.
    driver_ftable ftable;
} driver_header;