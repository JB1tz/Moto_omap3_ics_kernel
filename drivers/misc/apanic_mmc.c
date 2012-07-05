/* drivers/misc/apanic_mmc.c
 *
 * Copyright (C) 2009 Google, Inc.
 * Copyright (C) 2010 Motorola, Inc.
 * Author: San Mehat <san@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 */

#include <asm/setup.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/wakelock.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/notifier.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/rtc.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/console.h>
#include <linux/preempt.h>
#include <linux/completion.h>
#include <linux/bio.h>
#include <linux/mmc/core.h>

#include "apanic_mmc.h"

#define MEM_DUMP_LABEL "memdump"

struct panic_header {
	u32 magic;
#define PANIC_MAGIC 0xdeadf00d

	u32 version;
#define PHDR_VERSION   0x01

	u32 console_offset;
	u32 console_length;

	u32 threads_offset;
	u32 threads_length;
};

struct memdump_header {
#define MEMDUMP_MAGIC_LEN 8
	char magic[MEMDUMP_MAGIC_LEN];
#define MEMDUMP_MAGIC "MEM-DUMP"
	u32 version;
	struct timespec ts;
	/* dump contents */
	u32 sdram_offset;
	u32 sdram_length;
	u32 sram_offset;
	u32 sram_length;
};

struct apanic_data {
	struct panic_header	curr;
	void			*bounce;
	struct proc_dir_entry	*apanic_console;
	struct proc_dir_entry	*apanic_threads;
	struct raw_hd_struct    *mmchd;
	struct raw_mmc_panic_ops	*mmc_panic_ops;
};

struct memdump_data {
	struct raw_hd_struct    *memdump_hd;
	struct raw_mmc_panic_ops	*mmc_memdump_ops;
};

static struct apanic_data drv_ctx;
static struct work_struct proc_removal_work;
/* avoid collision of proc interface operation and proc removal */
static DEFINE_MUTEX(drv_mutex);

static char apanic_dev_name[BDEVNAME_SIZE];

#ifdef CONFIG_APANIC_MMC_MEMDUMP
static struct memdump_data memdump_ctx;
static char memdump_dev_name[BDEVNAME_SIZE];
#endif

#define APANIC_INVALID_OFFSET 0xFFFFFFFF

static void mmc_bio_complete(struct bio *bio, int err)
{
	complete((struct completion *)bio->bi_private);
}

static int apanic_proc_read_mmc(char *buffer, char **start, off_t offset,
			       int count, int *peof, void *dat)
{
	int i, index = 0;
	int ret;
	int start_sect;
	int end_sect;
	size_t file_length;
	off_t file_offset;
	dev_t devid;
	struct apanic_data *ctx = &drv_ctx;
	struct block_device *bdev;
	struct bio bio;
	struct bio_vec bio_vec;
	struct completion complete;
	struct page *page;

	if (!count)
		return 0;

	mutex_lock(&drv_mutex);

	switch ((int) dat) {
	case 1:	/* apanic_console */
		file_length = ctx->curr.console_length;
		file_offset = ctx->curr.console_offset;
		break;
	case 2:	/* apanic_threads */
		file_length = ctx->curr.threads_length;
		file_offset = ctx->curr.threads_offset;
		break;
	default:
		pr_err("Bad dat (%d)\n", (int) dat);
		mutex_unlock(&drv_mutex);
		return -EINVAL;
	}

	if ((offset + count) > file_length) {
		mutex_unlock(&drv_mutex);
		return 0;
	}

	devid = MKDEV(ctx->mmchd->major, ctx->mmchd->first_minor +
		ctx->mmchd->partno);
	bdev = open_by_devnum(devid, FMODE_READ);
	if (IS_ERR(bdev)) {
		printk(KERN_ERR "apanic: open device failed with %ld\n",
			PTR_ERR(bdev));
		ret = PTR_ERR(bdev);
		goto out_err;
	}
	page = virt_to_page(ctx->bounce);

	start_sect = (file_offset +  offset) / 512;
	end_sect = (file_offset + offset + count - 1) / 512;

	for (i = start_sect; i <= end_sect; i++) {
		bio_init(&bio);
		bio.bi_io_vec = &bio_vec;
		bio_vec.bv_page = page;
		bio_vec.bv_len = 512;
		bio_vec.bv_offset = 0;
		bio.bi_vcnt = 1;
		bio.bi_idx = 0;
		bio.bi_size = 512;
		bio.bi_bdev = bdev;
		bio.bi_sector = i;
		init_completion(&complete);
		bio.bi_private = &complete;
		bio.bi_end_io = mmc_bio_complete;
		submit_bio(READ, &bio);
		wait_for_completion(&complete);
		if (!test_bit(BIO_UPTODATE, &bio.bi_flags)) {
			ret = -EIO;
			goto out_err;
		}

		if ((i == start_sect) && ((file_offset + offset) % 512 != 0)) {
			/* first sect, may be the only sect */
			memcpy(buffer, ctx->bounce + (file_offset + offset)
				% 512, min((unsigned long)count,
				(unsigned long)
				(512 - (file_offset + offset) % 512)));
			index += min((unsigned long)count, (unsigned long)
				(512 - (file_offset + offset) % 512));
		} else if ((i == end_sect) && ((file_offset + offset + count)
			% 512 != 0)) {
			/* last sect */
			memcpy(buffer + index, ctx->bounce, (file_offset +
				offset + count) % 512);
		} else {
			/* middle sect */
			memcpy(buffer + index, ctx->bounce, 512);
			index += 512;
		}
	}
	blkdev_put(bdev, FMODE_READ);

	*start = (char *)count;

	if ((offset + count) == file_length)
		*peof = 1;

	mutex_unlock(&drv_mutex);
	return count;

out_err:
	mutex_unlock(&drv_mutex);
	return ret;
}

static void mmc_panic_erase(void)
{
	int i = 0;
	dev_t devid;
	struct apanic_data *ctx = &drv_ctx;
	struct block_device *bdev;
	struct bio bio;
	struct bio_vec bio_vec;
	struct completion complete;
	struct page *page;

	devid = MKDEV(ctx->mmchd->major, ctx->mmchd->first_minor +
		ctx->mmchd->partno);
	bdev = open_by_devnum(devid, FMODE_WRITE);
	if (IS_ERR(bdev)) {
		printk(KERN_ERR "apanic: open device failed with %ld\n",
			PTR_ERR(bdev));
		goto out_err;
	}
	page = virt_to_page(ctx->bounce);
	memset(ctx->bounce, 0, PAGE_SIZE);

	while (i < ctx->mmchd->nr_sects) {
		bio_init(&bio);
		bio.bi_io_vec = &bio_vec;
		bio_vec.bv_offset = 0;
		bio_vec.bv_page = page;
		bio.bi_vcnt = 1;
		bio.bi_idx = 0;
		bio.bi_sector = i;
		if (ctx->mmchd->nr_sects - i >= 8) {
			bio_vec.bv_len = PAGE_SIZE;
			bio.bi_size = PAGE_SIZE;
			i += 8;
		} else {
			bio_vec.bv_len = (ctx->mmchd->nr_sects - i) * 512;
			bio.bi_size = (ctx->mmchd->nr_sects - i) * 512;
			i = ctx->mmchd->nr_sects;
		}
		bio.bi_bdev = bdev;
		init_completion(&complete);
		bio.bi_private = &complete;
		bio.bi_end_io = mmc_bio_complete;
		submit_bio(WRITE, &bio);
		wait_for_completion(&complete);
	}
	blkdev_put(bdev, FMODE_WRITE);
out_err:
	return;
}

static void apanic_remove_proc_work(struct work_struct *work)
{
	struct apanic_data *ctx = &drv_ctx;

	mutex_lock(&drv_mutex);
	mmc_panic_erase();
	memset(&ctx->curr, 0, sizeof(struct panic_header));
	if (ctx->apanic_console) {
		remove_proc_entry("apanic_console", NULL);
		ctx->apanic_console = NULL;
	}
	if (ctx->apanic_threads) {
		remove_proc_entry("apanic_threads", NULL);
		ctx->apanic_threads = NULL;
	}
	mutex_unlock(&drv_mutex);
}

static int apanic_proc_write(struct file *file, const char __user *buffer,
		unsigned long count, void *data)
{
	schedule_work(&proc_removal_work);
	return count;
}

static void mmc_panic_notify_add(struct raw_hd_struct *hd,
			struct raw_mmc_panic_ops *panic_ops)
{
	dev_t devid;
	struct apanic_data *ctx = &drv_ctx;
	struct panic_header *hdr = ctx->bounce;
	struct block_device *bdev;
	struct bio bio;
	struct bio_vec bio_vec;
	struct completion complete;
	struct page *page;

	ctx->mmchd = hd;
	ctx->mmc_panic_ops = panic_ops;

	devid = MKDEV(hd->major, hd->first_minor + hd->partno);
	bdev = open_by_devnum(devid, FMODE_READ);
	if (IS_ERR(bdev)) {
		printk(KERN_ERR "apanic: open device failed with %ld\n",
			PTR_ERR(bdev));
		goto out_err;
	}
	page = virt_to_page(ctx->bounce);

	bio_init(&bio);
	bio.bi_io_vec = &bio_vec;
	bio_vec.bv_page = page;
	bio_vec.bv_len = PAGE_SIZE;
	bio_vec.bv_offset = 0;
	bio.bi_vcnt = 1;
	bio.bi_idx = 0;
	bio.bi_size = PAGE_SIZE;
	bio.bi_bdev = bdev;
	bio.bi_sector = 0;
	init_completion(&complete);
	bio.bi_private = &complete;
	bio.bi_end_io = mmc_bio_complete;
	submit_bio(READ, &bio);
	wait_for_completion(&complete);

	blkdev_put(bdev, FMODE_READ);
	printk(KERN_ERR "apanic: Bound to mmc block device '%s(%d:%d)'\n",
		apanic_dev_name, hd->major, hd->first_minor + hd->partno);

	if (hdr->magic != PANIC_MAGIC) {
		printk(KERN_INFO "apanic: No panic data available\n");
		return;
	}

	if (hdr->version != PHDR_VERSION) {
		printk(KERN_INFO "apanic: Version mismatch (%d != %d)\n",
		       hdr->version, PHDR_VERSION);
		return;
	}

	memcpy(&ctx->curr, hdr, sizeof(struct panic_header));

	printk(KERN_INFO "apanic: c(%u, %u) t(%u, %u)\n",
	       hdr->console_offset, hdr->console_length,
	       hdr->threads_offset, hdr->threads_length);

	if (hdr->console_length) {
		ctx->apanic_console = create_proc_entry("apanic_console",
						      S_IFREG | S_IRUGO, NULL);
		if (!ctx->apanic_console)
			printk(KERN_ERR "%s: failed creating procfile\n",
			       __func__);
		else {
			ctx->apanic_console->read_proc = apanic_proc_read_mmc;
			ctx->apanic_console->write_proc = apanic_proc_write;
			ctx->apanic_console->size = hdr->console_length;
			ctx->apanic_console->data = (void *) 1;
		}
	}

	if (hdr->threads_length) {
		ctx->apanic_threads = create_proc_entry("apanic_threads",
						       S_IFREG | S_IRUGO, NULL);
		if (!ctx->apanic_threads)
			printk(KERN_ERR "%s: failed creating procfile\n",
			       __func__);
		else {
			ctx->apanic_threads->read_proc = apanic_proc_read_mmc;
			ctx->apanic_threads->write_proc = apanic_proc_write;
			ctx->apanic_threads->size = hdr->threads_length;
			ctx->apanic_threads->data = (void *) 2;
		}
	}

	return;
out_err:
	ctx->mmchd = NULL;
	return;
}

static void mmc_panic_notify_remove(struct raw_hd_struct *hd)
{
	struct apanic_data *ctx = &drv_ctx;
	if (hd == ctx->mmchd) {
		ctx->mmchd = NULL;
		ctx->mmc_panic_ops = NULL;
		printk(KERN_INFO "apanic: Unbound from %s\n", apanic_dev_name);
	}
}

static struct mmcpart_notifier mmc_panic_notifier = {
	.add	= mmc_panic_notify_add,
	.remove	= mmc_panic_notify_remove,
};

#ifdef CONFIG_APANIC_MMC_MEMDUMP
static void mmc_memdump_notify_add(struct raw_hd_struct *hd,
			struct raw_mmc_panic_ops *panic_ops)
{
	/* no need to disturb the backing device now */
	memdump_ctx.memdump_hd = hd;
	memdump_ctx.mmc_memdump_ops = panic_ops;

	printk(KERN_INFO "apanic: memdump backing device set to '%s(%d:%d)',"
		" old dump always get overwritten\n",
		memdump_dev_name, hd->major, hd->first_minor + hd->partno);

	return;
}

static void mmc_memdump_notify_remove(struct raw_hd_struct *hd)
{
	if (hd == memdump_ctx.memdump_hd) {
		memdump_ctx.memdump_hd = NULL;
		memdump_ctx.mmc_memdump_ops = NULL;
		printk(KERN_INFO "apanic: Unbound from memdump backing dev"
			" %s\n", apanic_dev_name);
	}
}

static struct mmcpart_notifier mmc_memdump_notifier = {
	.add	= mmc_memdump_notify_add,
	.remove	= mmc_memdump_notify_remove,
};
#endif /* CONFIG_APANIC_MMC_MEMDUMP */

static int in_panic;

/*
 * Writes the contents of the console to the specified offset in mmc.
 * Returns number of bytes written
 */
static int apanic_write_console_mmc(unsigned int off)
{
	struct apanic_data *ctx = &drv_ctx;
	int saved_oip;
	int idx = 0;
	int rc, rc2;
	unsigned int last_chunk = 0;

	while (!last_chunk) {
		saved_oip = oops_in_progress;
		oops_in_progress = 1;
		rc = log_buf_copy(ctx->bounce, idx, PAGE_SIZE);
		if (rc < 0)
			break;

		if (rc != PAGE_SIZE)
			last_chunk = rc;

		oops_in_progress = saved_oip;
		if (rc <= 0)
			break;
		if (rc != PAGE_SIZE)
			memset(ctx->bounce + rc, 0, PAGE_SIZE - rc);

		rc2 = ctx->mmc_panic_ops->panic_write(ctx->mmchd, ctx->bounce,
			off, rc);
		if (rc2 <= 0) {
			printk(KERN_EMERG
			       "apanic: Flash write failed (%d)\n", rc2);
			return idx;
		}
		if (!last_chunk)
			idx += rc2;
		else
			idx += last_chunk;
		off += rc2;
	}
	return idx;
}

static void apanic_mmc_logbuf_dump(void)
{
	struct apanic_data *ctx = &drv_ctx;
	struct panic_header *hdr = (struct panic_header *) ctx->bounce;
	int console_offset = 0;
	int console_len = 0;
	int threads_offset = 0;
	int threads_len = 0;
	int rc;
	struct timespec now;
	struct timespec uptime;
	struct rtc_time rtc_timestamp;
	struct console *con;

	if (!ctx->mmchd || !ctx->mmc_panic_ops ||
			!ctx->mmc_panic_ops->panic_probe)
		return;
	if (ctx->mmc_panic_ops->panic_probe(ctx->mmchd,
			ctx->mmc_panic_ops->type)) {
		printk(KERN_ERR "apanic: choose to use mmc, "
			"but eMMC card not detected\n");
		return;
	}
	console_offset = 1024;

	if (ctx->curr.magic) {
		printk(KERN_EMERG "Crash partition in use!\n");
		return;
	}

	/*
	 * Add timestamp to displays current UTC time and uptime (in seconds).
	 */
	now = current_kernel_time();
	rtc_time_to_tm((unsigned long)now.tv_sec, &rtc_timestamp);
	do_posix_clock_monotonic_gettime(&uptime);
	bust_spinlocks(1);
	printk(KERN_EMERG "Timestamp = %lu.%03lu\n",
			(unsigned long)now.tv_sec,
			(unsigned long)(now.tv_nsec / 1000000));
	printk(KERN_EMERG "Current Time = "
			"%02d-%02d %02d:%02d:%lu.%03lu, "
			"Uptime = %lu.%03lu seconds\n",
			rtc_timestamp.tm_mon + 1, rtc_timestamp.tm_mday,
			rtc_timestamp.tm_hour, rtc_timestamp.tm_min,
			(unsigned long)rtc_timestamp.tm_sec,
			(unsigned long)(now.tv_nsec / 1000000),
			(unsigned long)uptime.tv_sec,
			(unsigned long)(uptime.tv_nsec/USEC_PER_SEC));
	bust_spinlocks(0);


	/*
	 * Write out the console
	 */
	console_len = apanic_write_console_mmc(console_offset);
	if (console_len < 0) {
		printk(KERN_EMERG "Error writing console to panic log! (%d)\n",
		       console_len);
		console_len = 0;
	}

	/*
	 * Write out all threads
	 */
	threads_offset = (ALIGN(console_offset + console_len,
		1024) == 0) ? 1024 :
		ALIGN(console_offset + console_len, 1024);

	log_buf_clear();

	for (con = console_drivers; con; con = con->next)
		con->flags &= ~CON_ENABLED;

	show_state_filter(0);
	threads_len = apanic_write_console_mmc(threads_offset);
	if (threads_len < 0) {
		printk(KERN_EMERG "Error writing threads to panic log! (%d)\n",
		       threads_len);
		threads_len = 0;
	}

	for (con = console_drivers; con; con = con->next)
		con->flags |= CON_ENABLED;
	/*
	 * Finally write the panic header
	 */
	memset(ctx->bounce, 0, PAGE_SIZE);
	hdr->magic = PANIC_MAGIC;
	hdr->version = PHDR_VERSION;

	hdr->console_offset = console_offset;
	hdr->console_length = console_len;

	hdr->threads_offset = threads_offset;
	hdr->threads_length = threads_len;

	rc = ctx->mmc_panic_ops->panic_write(ctx->mmchd, ctx->bounce, 0,
		console_offset);
	if (rc <= 0) {
		printk(KERN_EMERG "apanic: Header write failed (%d)\n",
		       rc);
		return;
	}

	printk(KERN_EMERG "apanic: Panic dump successfully written\n");
}

#ifdef CONFIG_APANIC_MMC_MEMDUMP
static void apanic_mmc_memdump(void)
{
	int rc;
	struct membank *bank;
	struct apanic_data *ctx = &drv_ctx;
	struct memdump_header *hdr = (struct memdump_header *)drv_ctx.bounce;

	if (!memdump_ctx.memdump_hd || !memdump_ctx.mmc_memdump_ops ||
			!memdump_ctx.mmc_memdump_ops->panic_probe)
		return;

	memdump_wdt_disable();
	if (memdump_ctx.mmc_memdump_ops->panic_probe(memdump_ctx.memdump_hd,
			memdump_ctx.mmc_memdump_ops->type)) {
		printk(KERN_ERR "apanic: full memeory dump backing device"
			" not detected, fail to do memory dump\n");
		return;
	}

	memset(ctx->bounce, 0, PAGE_SIZE);
	rc = memdump_ctx.mmc_memdump_ops->panic_write(memdump_ctx.memdump_hd,
		ctx->bounce, 0, PAGE_SIZE);
	if (rc <= 0) {
		printk(KERN_EMERG "apanic: memdump erase header failed (%d)\n",
		       rc);
		return;
	}

	/* no need to flush_cache_all(); */
	/* get the last memory bank, probably the only bank */
	if (meminfo.nr_banks >= 1 && meminfo.nr_banks < NR_BANKS)
		bank = &meminfo.bank[meminfo.nr_banks - 1];

	rc = memdump_ctx.mmc_memdump_ops->panic_write(memdump_ctx.memdump_hd,
		__va(bank->start), PAGE_SIZE, bank->size);
	if (rc <= 0) {
		printk(KERN_EMERG "apanic: full memory write failed rc=%d)\n",
			rc);
		return;
	}

	strncpy(hdr->magic, MEMDUMP_MAGIC, MEMDUMP_MAGIC_LEN);
	hdr->version = PHDR_VERSION;
	hdr->ts = current_kernel_time();
	hdr->sdram_offset = PAGE_SIZE;
	hdr->sdram_length = bank->size;

	rc = memdump_ctx.mmc_memdump_ops->panic_write(memdump_ctx.memdump_hd,
		ctx->bounce, 0, PAGE_SIZE);
	if (rc <= 0) {
		printk(KERN_EMERG "apanic: memdump header write failed (%d)\n",
		       rc);
		return;
	}

	printk(KERN_EMERG "apanic: full memory dump successfully written\n");
}
#endif /* CONFIG_APANIC_MMC_MEMDUMP */

static int apanic_mmc(struct notifier_block *this, unsigned long event,
			void *ptr)
{
	if (in_panic)
		return NOTIFY_DONE;
	in_panic = 1;
#ifdef CONFIG_PREEMPT
	/* Ensure that cond_resched() won't try to preempt anybody */
	add_preempt_count(PREEMPT_ACTIVE);
#endif
	touch_softlockup_watchdog();

	apanic_mmc_logbuf_dump();

#ifdef CONFIG_APANIC_MMC_MEMDUMP
	apanic_mmc_memdump();
#endif

#ifdef CONFIG_PREEMPT
	sub_preempt_count(PREEMPT_ACTIVE);
#endif
	in_panic = 0;
	return NOTIFY_DONE;
}

static struct notifier_block panic_blk = {
	.notifier_call	= apanic_mmc,
};

static int panic_dbg_get(void *data, u64 *val)
{
	apanic_mmc(NULL, 0, NULL);
	return 0;
}

static int panic_dbg_set(void *data, u64 val)
{
	BUG();
	return -1;
}

DEFINE_SIMPLE_ATTRIBUTE(panic_dbg_fops, panic_dbg_get, panic_dbg_set, "%llu\n");

static int __init apanic_mmc_init(void)
{
	int ret;

	/* listen to kpanic partition */
	ret = get_mmcpart_by_name(CONFIG_APANIC_PLABEL, apanic_dev_name);
	if (!ret) {
		strncpy(mmc_panic_notifier.partname, CONFIG_APANIC_PLABEL,
			BDEVNAME_SIZE);
		mmc_panic_notifier.partname[BDEVNAME_SIZE - 1] = '\0';
		register_mmcpart_user(&mmc_panic_notifier);
	} else
		printk(KERN_ERR "apanic: %s partition not on eMMC\n",
			CONFIG_APANIC_PLABEL);

#ifdef CONFIG_APANIC_MMC_MEMDUMP
	/* listen to memdump partition */
	ret = get_mmcpart_by_name(MEM_DUMP_LABEL, memdump_dev_name);
	if (!ret) {
		printk(KERN_INFO "apanic: %s selected as memdump device\n",
			MEM_DUMP_LABEL);
		strncpy(mmc_memdump_notifier.partname, MEM_DUMP_LABEL,
			BDEVNAME_SIZE);
		mmc_memdump_notifier.partname[BDEVNAME_SIZE - 1] = '\0';
		register_mmcpart_user(&mmc_memdump_notifier);
	}
#endif

	atomic_notifier_chain_register(&panic_notifier_list, &panic_blk);
	debugfs_create_file("apanic_mmc", 0644, NULL, NULL, &panic_dbg_fops);
	memset(&drv_ctx, 0, sizeof(drv_ctx));
	drv_ctx.bounce = (void *) __get_free_page(GFP_KERNEL);
	INIT_WORK(&proc_removal_work, apanic_remove_proc_work);
	printk(KERN_INFO "Android kernel panic mmc handler initialized\n");

	return 0;
}

module_init(apanic_mmc_init);
