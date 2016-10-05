/*
 * blk_daq.h
 *
 *  Created on: 2016å¹?æœ?æ—? *      Author: zhuce
 */

#ifndef BLK_DAQ_H_
#define BLK_DAQ_H_

#include <linux/ioctl.h>
#include <linux/config.h>
#include <linux/module.h>
//#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/timer.h>
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>
#include <linux/blkdev.h>

#include "my_printk.h"

enum {
	BLK_DAQ_CMD_NORMAL_READ = 0,
	BLK_DAQ_CMD_NORMAL_WRITE,
	BLK_DAQ_CMD_EXTERNAL_WRITE
};

struct blk_daq_cmd {
	unsigned char cmd_type;
	u64 sector;
	u64 size;
};

struct blk_daq_dev {
	int dev_major;
	int index;

	unsigned int hardsect_size;
	u64 nsectors;//int nsectors;	/* How big the drive is */

	u64 size;                       /* Device size in sectors */
	short users;  //atomic_t is better   /* How many users */
	short media_change;             /* Flag a media change? */
	spinlock_t lock;                /* For mutual exclusion */
	struct request_queue *queue;    /* The device request queue */
	struct gendisk *gd;             /* The gendisk structure */
	struct timer_list timer;        /* For simulated media changes */

	atomic_t aWrite;
};

extern int blk_daq_init(void);
extern int blk_daq_exit(void);
extern int blk_daq_add_device(struct blk_daq_dev *dev);
extern int blk_daq_del_device(struct blk_daq_dev *dev);


#endif /* BLK_DAQ_H_ */
