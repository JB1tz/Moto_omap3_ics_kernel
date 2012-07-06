/*
 * PVR-off: module to unload built-in SGX PVR drivers to be able to replace
 * them with updated version loaded as a module (for kernel-locked devices).
 * Depends on symsearch module by Skrilax_CZ
 * 
 * Copyright (C) 2012 Nadlabak
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/smp_lock.h>
#include "symsearch.h"

#include "syscommon.h"

struct driver_private {
         struct kobject kobj;
         struct klist klist_devices;
         struct klist_node knode_bus;
         struct module_kobject *mkobj;
         struct device_driver *driver;
};

typedef enum _OMAP_ERROR_
{
	OMAP_OK                             =  0,
	OMAP_ERROR_GENERIC                  =  1,
	OMAP_ERROR_OUT_OF_MEMORY            =  2,
	OMAP_ERROR_TOO_FEW_BUFFERS          =  3,
	OMAP_ERROR_INVALID_PARAMS           =  4,
	OMAP_ERROR_INIT_FAILURE             =  5,
	OMAP_ERROR_CANT_REGISTER_CALLBACK   =  6,
	OMAP_ERROR_INVALID_DEVICE           =  7,
	OMAP_ERROR_DEVICE_REGISTER_FAILED   =  8
} OMAP_ERROR;

#define	LDM_DEV	struct platform_device

SYMSEARCH_DECLARE_FUNCTION_STATIC(OMAP_ERROR, OMAPLFBDeinit);
SYMSEARCH_DECLARE_FUNCTION_STATIC(IMG_INT, PVRSRVDriverRemove, LDM_DEV *);
SYMSEARCH_DECLARE_FUNCTION_STATIC(IMG_VOID, PVRMMapCleanup);
SYMSEARCH_DECLARE_FUNCTION_STATIC(IMG_VOID, LinuxMMCleanup);
SYMSEARCH_DECLARE_FUNCTION_STATIC(IMG_VOID, LinuxBridgeDeInit);
SYMSEARCH_DECLARE_FUNCTION_STATIC(IMG_VOID, RemoveProcEntries);
SYMSEARCH_DECLARE_ADDRESS_STATIC(rfkill_fops);

static int __exit OMAPLFBDriverRemove_Entry(struct platform_device *pdev)
{
	if(OMAPLFBDeinit() != OMAP_OK)
		printk(KERN_WARNING "PVR-off: OMAPLFBDriverRemove: OMAPLFBDeinit failed\n");

	return 0;
}

static int platform_drv_remove_omaplfb(struct device *_dev)
{
         struct platform_device *dev = to_platform_device(_dev); 
         return OMAPLFBDriverRemove_Entry(dev);
}

static struct platform_device *pdev;

static int platform_drv_remove_pvrsrvkm(struct device *_dev)
{
         pdev = to_platform_device(_dev);
         return PVRSRVDriverRemove(pdev);
}

static IMG_VOID PVRSRVDeviceRelease(struct device *pDevice)
{
	PVR_UNREFERENCED_PARAMETER(pDevice);
}

static struct class *psPvrClass;

static int find_pvr_class_struct(void)
{
	/* rfkill_fops - the last symbol in kallsyms before the strings */
	unsigned char *func = (void *)SYMSEARCH_GET_ADDRESS(rfkill_fops);
	uint pvrClassNameAdr;
	uint match0 = 0, match1 = 0, match2 = 0, match3 = 0;
	int i;

	/* find address of 'pvr' string */
	for (i = 0; i < 0x00100000; i+=1) {
		// looking for ' pvr '
		if (func[i] == 0x00 && func[i+1] == 0x70 &&
		    func[i+2] == 0x76 && func[i+3] == 0x72 &&
		    func[i+4] == 0x00) {
			pvrClassNameAdr = (uint)func + i + 1;
			printk(KERN_INFO "PVR-off: found pvr class name string at 0x%x\n", pvrClassNameAdr);
			match0 = pvrClassNameAdr & 0xff;
			match1 = (pvrClassNameAdr & 0xff00) >> 8;
			match2 = (pvrClassNameAdr & 0xff0000) >> 16;
			match3 = (pvrClassNameAdr & 0xff000000) >> 24;
			break;
		}
	}

	if (!pvrClassNameAdr) {
		printk(KERN_WARNING "PVR-off: pvr class name string not found!\n");
		return -1;
	}

	/* the class structure begins with pointer to the name string */
	func = (void *)SGX_BASE_ADDR; /* 0xcec00000 on omap3410 */

	for(i = 0; i < 0x30000000; i+=1) {
		if (func[i+3] == match3 && func[i+2] == match2 &&
		    func[i+1] == match1 && func[i] == match0) {
			psPvrClass = (void *)((uint)func + i);
			printk(KERN_INFO "PVR-off: found pvr class struct at 0x%p\n", psPvrClass);
			printk(KERN_INFO "PVR-off: name of the found class is %s\n", psPvrClass->name);
			break;
		}
	}
	if (!psPvrClass) {
	    printk(KERN_WARNING "PVR-off: pvr class structure not found!\n");
	    return -1;
	}

	return 0;
}

static int __init pvroff_init(void)
{
	struct device_driver *drv;
	struct kobject *kobj;
	IMG_INT AssignedMajorNumber = 247;
    
	printk(KERN_INFO "PVR-off: init\n");
	SYMSEARCH_BIND_FUNCTION(pvr-off, OMAPLFBDeinit);
	SYMSEARCH_BIND_FUNCTION(pvr-off, PVRSRVDriverRemove);
	SYMSEARCH_BIND_FUNCTION(pvr-off, PVRMMapCleanup);
	SYMSEARCH_BIND_FUNCTION(pvr-off, LinuxMMCleanup);
	SYMSEARCH_BIND_FUNCTION(pvr-off, LinuxBridgeDeInit);
	SYMSEARCH_BIND_FUNCTION(pvr-off, RemoveProcEntries);
	SYMSEARCH_BIND_ADDRESS(pvr-off, rfkill_fops);

	lock_kernel();

	// OMAPLFB
	drv = driver_find("omaplfb", &platform_bus_type);
	if (!drv) {
		printk(KERN_WARNING "PVR-off: omaplfb driver not found, bailing out!\n");
		unlock_kernel();
		return -1;
	} else {
		kobj = &drv->p->kobj;
		drv->remove = platform_drv_remove_omaplfb;
		driver_unregister(drv);
		kobject_del(kobj);
	}

	// PVR
	if (find_pvr_class_struct()) {
		printk(KERN_WARNING "PVR-off: class structure search failed, bailing out!\n");
		unlock_kernel();
		return -1;
	}

	device_destroy(psPvrClass, MKDEV(AssignedMajorNumber, 0));
	class_destroy(psPvrClass);
	unregister_chrdev((IMG_UINT)AssignedMajorNumber, "pvrsrvkm");
	drv = driver_find("pvrsrvkm", &platform_bus_type);
	if (!drv) {
		printk(KERN_WARNING "PVR-off: pvrsrvkm driver not found, bailing out!\n");
		unlock_kernel();
		return -1;
	} else {
		kobj = &drv->p->kobj;
		drv->remove = platform_drv_remove_pvrsrvkm;
		driver_unregister(drv);
		kobject_del(kobj);
	}

	pdev->dev.release = PVRSRVDeviceRelease;
	platform_device_unregister(pdev);
	PVRMMapCleanup();
	LinuxMMCleanup();
	LinuxBridgeDeInit();
	RemoveProcEntries();
   	unlock_kernel();

	printk(KERN_INFO "PVR-off: successfully done\n");
	return 0;
}

static void __exit pvroff_exit(void)
{
}

module_init(pvroff_init);
module_exit(pvroff_exit);

MODULE_ALIAS("PVR-off");
MODULE_DESCRIPTION("unload built-in pvr and omaplfb drivers");
MODULE_AUTHOR("Nadlabak");
MODULE_LICENSE("GPL");
