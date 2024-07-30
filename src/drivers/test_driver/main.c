#include <int.h>
#include <klog.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <scheduler/thread.h>

static OBOS_PAGEABLE_VARIABLE driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = 0,
};

driver_id* this_driver;

OBOS_PAGEABLE_FUNCTION DRV_EXPORT void TestDriver_Test(driver_id* caller)
{
    OBOS_Log("Function in driver %d called from driver %d.\n", this_driver->id, caller->id);
}
extern char Drv_Base[];

OBOS_PAGEABLE_FUNCTION void OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    OBOS_Log("%s: Hello from test driver #1. Driver base: %p. Driver id: %d.\n", __func__, Drv_Base, this->id);
    OBOS_Log("Exiting from main thread.\n");
    Core_ExitCurrentThread();
}