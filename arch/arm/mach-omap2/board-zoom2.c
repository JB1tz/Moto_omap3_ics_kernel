/*
 * Copyright (C) 2009 Texas Instruments Inc.
 * Mikkel Christensen <mlc@ti.com>
 *
 * Modified from mach-omap2/board-ldp.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>

#include <plat/common.h>
#include <plat/board.h>

#include <mach/board-zoom.h>

#include "sdram-micron-mt46h32m32lf-6.h"
#include "omap3-opp.h"

static void __init omap_zoom2_init_irq(void)
{
	if (cpu_is_omap3630()) {
		omap2_init_common_hw(mt46h32m32lf6_sdrc_params,
			omap3630_mpu_rate_table, omap3630_dsp_rate_table,
			omap3630_l3_rate_table);
	} else {
		switch (omap_rev_id()) {
		case OMAP_3420:
			omap2_init_common_hw(mt46h32m32lf6_sdrc_params,
			omap3_mpu_rate_table, omap3_dsp_rate_table_3420,
			omap3_l3_rate_table);
			break;

		case OMAP_3430:
			omap2_init_common_hw(mt46h32m32lf6_sdrc_params,
			omap3_mpu_rate_table, omap3_dsp_rate_table,
			omap3_l3_rate_table);
			break;

		case OMAP_3440:
			omap2_init_common_hw(mt46h32m32lf6_sdrc_params,
			omap3_mpu_rate_table, omap3_dsp_rate_table_3440,
			omap3_l3_rate_table);
			break;

		default:
			omap2_init_common_hw(mt46h32m32lf6_sdrc_params,
			omap3_mpu_rate_table, omap3_dsp_rate_table,
			omap3_l3_rate_table);
			break;
		}
	}

	omap_init_irq();
	omap_gpio_init();
}

/* REVISIT: These audio entries can be removed once MFD code is merged */
#if 0

static struct twl4030_madc_platform_data zoom2_madc_data = {
	.irq_line	= 1,
};

static struct twl4030_platform_data zoom2_twldata = {
	.irq_base	= TWL4030_IRQ_BASE,
	.irq_end	= TWL4030_IRQ_END,

	/* platform_data for children goes here */
	.bci		= &zoom2_bci_data,
	.madc		= &zoom2_madc_data,
	.usb		= &zoom2_usb_data,
	.gpio		= &zoom2_gpio_data,
	.keypad		= &zoom2_kp_twl4030_data,
	.vmmc1          = &zoom2_vmmc1,
	.vmmc2          = &zoom2_vmmc2,
	.vsim           = &zoom2_vsim,

};

#endif

static void __init omap_zoom2_init(void)
{
	zoom_peripherals_init();
	zoom_debugboard_init();
}

static void __init omap_zoom2_map_io(void)
{
	omap2_set_globals_343x();
	omap2_map_common_io();
}

MACHINE_START(OMAP_ZOOM2, "OMAP Zoom2 board")
	.phys_io	= 0x48000000,
	.io_pg_offst	= ((0xfa000000) >> 18) & 0xfffc,
	.boot_params	= 0x80000100,
	.map_io		= omap_zoom2_map_io,
	.init_irq	= omap_zoom2_init_irq,
	.init_machine	= omap_zoom2_init,
	.timer		= &omap_timer,
MACHINE_END
