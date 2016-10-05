/*
 * blk_daq.c
 *
 *  Created on: 2016�?�?�? *      Author: zhuce
 */

//#include "usb_daq.h"
#include "blk_daq.h"
#include <linux/idr.h>
#include <linux/mutex.h>
#include "usb_trans.h"
#include <linux/cdrom.h>


/*
 * The different "request modes" we can use.
 */
enum {
	RM_SIMPLE  = 0,	/* The extra-simple request function */
	RM_FULL    = 1,	/* The full-blown version */
	RM_NOQUEUE = 2,	/* Use make_request */
};
static int request_mode = RM_NOQUEUE;

/*
 * Minor number and partition management.
 */
#define BLK_DAQ_MINORS		16
#define BLK_DAQ_SHIFT		4
//#define DEVNUM(kdevnum)	(MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE	512
#define KERNEL_SECTOR_SHIFT		9

#define INVALIDATE_DELAY	30*HZ

#ifndef blk_fs_request(rq)
#define blk_fs_request(rq) ((rq)->cmd_type == REQ_TYPE_FS)
#endif

#define USE_IDA

#ifdef USE_IDA
static DEFINE_SPINLOCK(bd_index_lock);
static DEFINE_IDA(bd_index_ida);
#endif

#define BLK_DAQ_IOCTL_START_WRITE		_IOWR('x', 0, char*)
#define BLK_DAQ_IOCTL_STOP_WRITE		_IOWR('x', 1, char*)

static int dev_major_t = 0;
static struct kref		blk_daq_kref;
static int bDel = 0;

/*
 * Handle an I/O request.
 */
static int blk_daq_transfer(struct blk_daq_dev *dev, unsigned long sector,
		unsigned long nsect, char *buffer, int write)
{
	int result;
	struct blk_daq_cmd *bdc;
	struct usb_daq_data *ud;
	unsigned long offset = sector*KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect*KERNEL_SECTOR_SIZE;
	unsigned long i = 0;

	if ((offset + nbytes) > dev->size) {
		my_printk("usb_daq: Beyond-end write (%lu %lu)\n", offset, nbytes);
		return USB_DAQ_TRANSPORT_ERROR;
	}

	ud = bd_to_ud(dev);
	bdc = (struct blk_daq_cmd *) ud->iobuf;
	bdc->cmd_type = write;
	bdc->sector = offset / dev->hardsect_size;
	bdc->size = nbytes/ dev->hardsect_size;
	unsigned int* pbt = (unsigned int*)buffer;

	my_printk("usb_daq: cmd_type %u, sector %llu, size %llu\n",
			bdc->cmd_type, bdc->sector, bdc->size);

	if (write && atomic_read(&(dev->aWrite)) == 1)
	{
		atomic_set(&(dev->aWrite), 0);
		bdc->cmd_type = BLK_DAQ_CMD_EXTERNAL_WRITE;
	}

	result = usb_daq_bulk_transfer_buf(ud, ud->send_blk_bulk_pipe,
										bdc, sizeof(*bdc), NULL);

	switch (bdc->cmd_type) {
	case BLK_DAQ_CMD_NORMAL_READ:
		result = usb_daq_bulk_transfer_buf(ud, ud->recv_blk_bulk_pipe,
							buffer, nbytes, NULL);
		//memcpy(buffer, dev->data + offset, nbytes);
		break;
	case BLK_DAQ_CMD_NORMAL_WRITE:
		result = usb_daq_bulk_transfer_buf(ud, ud->send_blk_bulk_pipe,
							buffer, nbytes, NULL);
		//memcpy(dev->data + offset, buffer, nbytes);
		break;
	case BLK_DAQ_CMD_EXTERNAL_WRITE:

		break;
	default:

		break;
	}

//			for (i = 0; i < nbytes >> 2; i += 8) {
//				if (i + 8 >= nbytes >> 2)
//					break;
//				my_printk("usb_daq: %08x %08x %08x %08x %08x %08x %08x %08x\n",
//						pbt[i], pbt[i+1], pbt[i+2], pbt[i+3], pbt[i+4], pbt[i+5], pbt[i+6], pbt[i+7]);
//			}

	my_printk("usb_daq: Bulk command transfer result=%d\n", result);
	if (result != USB_DAQ_XFER_GOOD)
		return USB_DAQ_TRANSPORT_ERROR;

	return result;//return USB_STOR_TRANSPORT_GOOD;
}

/*
 * The simple form of the request function.
 */
static void blk_daq_request(struct request_queue *q)
{
	struct request *req;

	//linux 2.6.10
	//while ((req = elv_next_request(q)) != NULL) {
	while ((req = blk_fetch_request(q)) != NULL) {
		struct blk_daq_dev *dev = req->rq_disk->private_data;
		if (! blk_fs_request(req)) {
			my_printk("usb_daq: Skip non-fs request\n");
			//linux 2.6.10
			//end_request(req,0);
			//blk_end_request_all
			blk_finish_request(req, 0);
			continue;
		}
    //    	my_printk("usb_daq: Req dev %d dir %ld sec %ld, nr %d f %lx\n",
    //    			dev - Devices, rq_data_dir(req),
    //    			req->sector, req->current_nr_sectors,
    //    			req->flags);
		//linux 2.6.10
//		blk_daq_transfer(dev, req->sector, req->current_nr_sectors,
//				req->buffer, rq_data_dir(req));
		blk_daq_transfer(dev, blk_rq_pos(req), blk_rq_cur_sectors(req),
				bio_data(req->bio), rq_data_dir(req));

		//linux 2.6.10
		//end_request(req,1);
		blk_finish_request(req, 1);
	}
}

/*
 * Transfer a single BIO.
 */
//static int sbull_xfer_bio(struct sbull_dev *dev, struct bio *bio)
//{
//	int i;
//	struct bio_vec *bvec;
//	sector_t sector = bio->bi_sector;
//
//	/* Do each segment independently. */
//	bio_for_each_segment(bvec, bio, i) {
//		  char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
//		  sbull_transfer(dev, sector, bio_cur_bytes(bio)>>9 ,
//		  buffer, bio_data_dir(bio) == WRITE);
//		  sector += bio_cur_bytes(bio)>>9;
//		  __bio_kunmap_atomic(bio, KM_USER0);
//	}
//	return 0; /* Always "succeed" */
//}
/*
 * Transfer a single BIO.
 */
static int blk_daq_xfer_bio(struct blk_daq_dev *dev, struct bio *bio)
{
	//int err;
	struct bio_vec bvec;
	struct bvec_iter iter;
	sector_t sector = bio->bi_iter.bi_sector;

	bio_for_each_segment(bvec, bio, iter) {
		char *buffer = __bio_kmap_atomic(bio, iter);
		unsigned int len = bvec.bv_len>>KERNEL_SECTOR_SHIFT;

		blk_daq_transfer(dev, sector, len/*bio_cur_bytes(bio)>>9*/,
						buffer, bio_data_dir(bio) == WRITE);

		sector += len;

		__bio_kunmap_atomic(bio);
	}


	//linux 2.6.10
//	/* Do each segment independently. */
//	bio_for_each_segment(bvec, bio, i) {
//		char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
//		blk_daq_transfer(dev, sector, bio_cur_sectors(bio),
//				buffer, bio_data_dir(bio) == WRITE);
//		sector += bio_cur_sectors(bio);
//		__bio_kunmap_atomic(bio, KM_USER0);
//	}
	return 0; /* Always "succeed" */
}

/*
 * Transfer a full request.
 */
static int blk_daq_xfer_request(struct blk_daq_dev *dev, struct request *req)
{
	struct bio *bio;
	int nsect = 0;

	//linux 2.6.10
	//rq_for_each_bio(bio, req) {
	__rq_for_each_bio(bio, req) {
		blk_daq_xfer_bio(dev, bio);
		nsect += bio_cur_bytes(bio)>>9;
	}
	//we can use another define as
	//rq_for_each_segment
	return nsect;
}



/*
 * Smarter request function that "handles clustering".
 */
static void blk_daq_full_request(struct request_queue *q)
{
//	struct request *req;
//	int sectors_xferred;
//	struct blk_daq_dev *dev = q->queuedata;
//
//	while ((req = elv_next_request(q)) != NULL) {
//		if (! blk_fs_request(req)) {
//			my_printk("usb_daq: Skip non-fs request\n");
//			end_request(req, 0);
//			continue;
//		}
//		sectors_xferred = blk_daq_xfer_request(dev, req);
//		if (! end_that_request_first(req, 1, sectors_xferred)) {
//			blkdev_dequeue_request(req);
//			end_that_request_last(req);
//		}
//	}
}



/*
 * The direct make request version.
 */
static blk_qc_t blk_daq_make_request(struct request_queue *q, struct bio *bio)
{
	struct blk_daq_dev *dev = q->queuedata;
	int status;

	status = blk_daq_xfer_bio(dev, bio);
	//linux 2.6.10
	//bio_endio(bio, bio->bi_size, status);
	bio_endio(bio);
	return 0;
}


/*
 * Open and close.
 */
//linux 2.6.10
//static int blk_daq_open(struct inode *inode, struct file *filp)
static int blk_daq_open(struct block_device *bdev, fmode_t mode)
{
	struct blk_daq_dev *dev = bdev->bd_disk->private_data;

	my_printk("usb_daq: enter %s\n", __func__);
	del_timer_sync(&dev->timer);
	spin_lock(&dev->lock);
	if (!dev->users)
	{
		check_disk_change(bdev);
		my_printk("usb_daq: %s check_disk_change\n", __func__);
	}
	dev->users++;
	atomic_set(&(dev->aWrite), 0);
	my_printk("usb_daq: %s users %d\n", __func__, dev->users);
	spin_unlock(&dev->lock);
	my_printk("usb_daq: exit %s\n", __func__);
	return 0;
}

//linux 2.6.10
//static int blk_daq_release(struct inode *inode, struct file *filp)
static void blk_daq_release(struct gendisk *disk, fmode_t mode)
{
	struct blk_daq_dev *dev = disk->private_data;

	my_printk("usb_daq: enter %s\n", __func__);
	spin_lock(&dev->lock);
	dev->users--;

	my_printk("usb_daq: %s users %d\n", __func__, dev->users);

	if (!dev->users && !bDel) {
		dev->timer.expires = jiffies + INVALIDATE_DELAY;
		add_timer(&dev->timer);
	}
	atomic_set(&(dev->aWrite), 0);

	spin_unlock(&dev->lock);
	my_printk("usb_daq: exit %s\n", __func__);
}

/*
 * Look for a (simulated) media change.
 */
int blk_daq_media_changed(struct gendisk *gd)
{
	struct blk_daq_dev *dev = gd->private_data;

	my_printk("usb_daq: enter %s\n", __func__);

	my_printk("usb_daq: exit %s\n", __func__);

	return dev->media_change;
}

/*
 * Revalidate.  WE DO NOT TAKE THE LOCK HERE, for fear of deadlocking
 * with open.  That needs to be reevaluated.
 */
int blk_daq_revalidate(struct gendisk *gd)
{
	struct blk_daq_dev *dev = gd->private_data;

	my_printk("usb_daq: enter %s\n", __func__);
	if (dev->media_change) {
		dev->media_change = 0;
		//memset (dev->data, 0, dev->size);
	}
	my_printk("usb_daq: exit %s\n", __func__);
	return 0;
}

/*
 * The "invalidate" function runs out of the device timer; it sets
 * a flag to simulate the removal of the media.
 */
void blk_daq_invalidate(unsigned long ldev)
{
	struct blk_daq_dev *dev = (struct blk_daq_dev *) ldev;

	my_printk("usb_daq: enter %s\n", __func__);
	spin_lock(&dev->lock);
	if (dev->users/* || !dev->data*/)
		my_printk("usb_daq: timer sanity check failed\n");
	else
		//dev->media_change = 1;
	spin_unlock(&dev->lock);
	my_printk("usb_daq: exit %s\n", __func__);
}

/*
 * The ioctl() implementation
 */
int blk_daq_ioctl(struct block_device *bdev, fmode_t mode, unsigned int cmd, unsigned long arg)
{
	u64 len = 0;
	struct blk_daq_dev *dev = bdev->bd_disk->private_data;

	my_printk("usb_daq: enter %s\n", __func__);

	my_printk("usb_daq: blk_daq_ioctl cmd %#x, arg %#lx\n", cmd, arg);
	switch (cmd) {
		case CDROM_GET_CAPABILITY:
			return -ENOIOCTLCMD;
			break;
		case BLK_DAQ_IOCTL_START_WRITE:
			atomic_inc(&(dev->aWrite));
			break;
		case BLK_DAQ_IOCTL_STOP_WRITE:
			//atomic_dec(&(dev->aWrite));
			if (!atomic_dec_and_test(&(dev->aWrite)))
			{
				return -ENOIOCTLCMD;
			}
			break;
		default:
			return -ENOIOCTLCMD;
			break;
	}

	my_printk("usb_daq: exit %s\n", __func__);
	//return 0;
	return 0;
}
//linux 2.6.10
//int blk_daq_ioctl (struct inode *inode, struct file *filp,
//                 unsigned int cmd, unsigned long arg)
//{
//	long size;
//	struct hd_geometry geo;
//	struct blk_daq_dev *dev = filp->private_data;
//
//	my_printk("usb_daq: enter %s\n", __func__);
//
//	switch(cmd) {
//	    case HDIO_GETGEO:
//        	/*
//		 * Get geometry: since we are a virtual device, we have to make
//		 * up something plausible.  So we claim 16 sectors, four heads,
//		 * and calculate the corresponding number of cylinders.  We set the
//		 * start of data at sector four.
//		 */
//		size = dev->size*(dev->hardsect_size/KERNEL_SECTOR_SIZE);
//		geo.cylinders = (size & ~0x3f) >> 6;
//		geo.heads = 4;
//		geo.sectors = 16;
//		geo.start = 4;
//		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
//			return -EFAULT;
//		return 0;
//	}
//
//	my_printk("usb_daq: exit %s\n", __func__);
//
//	return -ENOTTY; /* unknown command */
//}

int blk_daq_get_geo (struct block_device *bdev, struct hd_geometry *geo)
{
	long size;
	//struct hd_geometry geo;
	struct blk_daq_dev *dev = bdev->bd_disk->private_data;

	my_printk("usb_daq: enter %s\n", __func__);

        	/*
	 * Get geometry: since we are a virtual device, we have to make
	 * up something plausible.  So we claim 16 sectors, four heads,
	 * and calculate the corresponding number of cylinders.  We set the
	 * start of data at sector four.
	 */
	size = dev->size*(dev->hardsect_size/KERNEL_SECTOR_SIZE);
	geo->heads = 64;
	geo->sectors = 16;
	geo->start = get_start_sect(bdev);//4;
	geo->cylinders = (size & ~0x3ff) >> 10;
	my_printk("usb_daq: get_geo, heads %u, sectors %u, cylinders %u, start %lu\n",
			geo->heads, geo->sectors, geo->cylinders, geo->start);

//	if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
//		return -EFAULT;

	my_printk("usb_daq: exit %s\n", __func__);

	return 0; /* unknown command */
}

/*
 * The device operations structure.
 */
static struct block_device_operations blk_daq_ops = {
	.owner           	= THIS_MODULE,
	.open 	         	= blk_daq_open,
	.release 	 		= blk_daq_release,
	.media_changed   	= blk_daq_media_changed,
	.revalidate_disk 	= blk_daq_revalidate,
	.ioctl	         	= blk_daq_ioctl,
	//.compat_ioctl		= ?,
	//linux now
	.getgeo				= blk_daq_get_geo
};

static int blk_daq_format_disk_name(char *prefix, int index, char *buf, int buflen)
{
	const int base = 'z' - 'a' + 1;
	char *begin = buf + strlen(prefix);
	char *end = buf + buflen;
	char *p;
	int unit;

	p = end - 1;
	*p = '\0';
	unit = base;
	do {
		if (p == begin)
			return -EINVAL;
		*--p = 'a' + (index % unit);
		index = (index / unit) - 1;
	} while (index >= 0);

	memmove(begin, p, end - p);
	memcpy(buf, prefix, strlen(prefix));

	return 0;
}

/*
 * Set up our internal device.
 */
int blk_daq_add_device(struct blk_daq_dev *dev)
{
	int retval = 0;
	int dev_index = 0;

	my_printk("usb_daq: enter %s\n", __func__);

	if (dev_major_t == 0) {
		return -EBUSY;
	}

	bDel = 0;

	dev->dev_major = dev_major_t;
//	dev = kmalloc(sizeof (struct blk_daq_dev), GFP_KERNEL);
//	if (dev == NULL) {
//		unregister_blkdev(dev->dev_major, "blk_daq");
//		return -ENOMEM;
//	}

	dev->users = 0;

	dev->size = dev->nsectors*dev->hardsect_size;

	spin_lock_init(&dev->lock);

	/*
	 * The timer which "invalidates" the device.
	 */
	init_timer(&dev->timer);
	dev->timer.data = (unsigned long) dev;
	dev->timer.function = blk_daq_invalidate;

	/*
	 * The I/O queue, depending on whether we are using our own
	 * make_request function or not.
	 */
	switch (request_mode) {
	    case RM_NOQUEUE:
		dev->queue = blk_alloc_queue(GFP_KERNEL);
		if (dev->queue == NULL)
			return -ENOMEM;
		blk_queue_make_request(dev->queue, blk_daq_make_request);
		break;

	    case RM_FULL:
		dev->queue = blk_init_queue(blk_daq_full_request, &dev->lock);
		if (dev->queue == NULL)
			return -ENOMEM;
		break;

	    default:
	    	my_printk("usb_daq: Bad request mode %d, using simple\n", request_mode);
        	/* fall into.. */

	    case RM_SIMPLE:
		dev->queue = blk_init_queue(blk_daq_request, &dev->lock);
		if (dev->queue == NULL)
			return -ENOMEM;
		break;
	}
	//linux 2.6.10
	//blk_queue_hardsect_size(dev->queue, dev->hardsect_size);
	blk_queue_logical_block_size(dev->queue, dev->hardsect_size);
	blk_queue_physical_block_size(dev->queue, dev->hardsect_size);
	dev->queue->queuedata = dev;

	/*
	 * And the gendisk structure.
	 */
	dev->gd = alloc_disk(BLK_DAQ_MINORS);
	if (! dev->gd) {
		my_printk("usb_daq: alloc_disk failure\n");
		return -ENOMEM;
	}
#ifdef USE_IDA
	do {
		if (!ida_pre_get(&bd_index_ida, GFP_KERNEL))
			goto out_put;

		spin_lock(&bd_index_lock);
		retval = ida_get_new(&bd_index_ida, &dev_index);
		spin_unlock(&bd_index_lock);
	} while (retval == -EAGAIN);

	if (retval) {
		my_printk("usb_daq: sd_probe: memory exhausted.\n");
		goto out_put;
	}

	if (dev_index > 0xffff) {
		my_printk("usb_daq: dev_index > 0xffff.\n");
	}
#else
	dev_index = 0;
#endif
	retval = blk_daq_format_disk_name("bd", dev_index, dev->gd->disk_name, 32);
	if (retval) {
		my_printk("usb_daq: disk name length exceeded.\n");
		goto out_free_index;
	}

	dev->index = dev_index;
	my_printk("usb_daq: dev_index %d\n", dev_index);

	dev->gd->major = dev->dev_major;
	dev->gd->first_minor = ((dev_index & 0xffff) << BLK_DAQ_SHIFT);
	dev->gd->fops = &blk_daq_ops;
	dev->gd->queue = dev->queue;
	//dev->gd->flags = GENHD_FL_REMOVABLE | GENHD_FL_SUPPRESS_PARTITION_INFO;
	dev->gd->private_data = dev;

	set_capacity(dev->gd, dev->nsectors*(dev->hardsect_size/KERNEL_SECTOR_SIZE));

	add_disk(dev->gd);

	my_printk("usb_daq: exit %s\n", __func__);

	return 0;

 out_free_index:
#ifdef USE_IDA
	spin_lock(&bd_index_lock);
	ida_remove(&bd_index_ida, dev_index);
	spin_unlock(&bd_index_lock);
#endif
 out_put:
 	//del_gendisk(dev->gd);
	put_disk(dev->gd);

	return retval;
}

int blk_daq_del_device(struct blk_daq_dev *dev)
{
	my_printk("usb_daq: enter %s\n", __func__);

#ifdef USE_IDA
	spin_lock(&bd_index_lock);
	ida_remove(&bd_index_ida, dev->index);
	spin_unlock(&bd_index_lock);
#endif

	del_timer_sync(&dev->timer);
	bDel = 1;

	if (dev->gd) {
		del_gendisk(dev->gd);
		put_disk(dev->gd);
	}
	if (dev->queue) {
//		linux 2.6.10
//		if (request_mode == RM_NOQUEUE)
//			blk_put_queue(dev->queue);
//		else
//			blk_cleanup_queue(dev->queue);
		blk_cleanup_queue(dev->queue);
	}

	my_printk("usb_daq: exit %s\n", __func__);

	return 0;
}

static void blk_daq_delete(struct kref *kref)
{
	my_printk("usb_daq: enter %s\n", __func__);
	unregister_blkdev(dev_major_t, "blk_daq");
	dev_major_t = 0;
	my_printk("usb_daq: exit %s\n", __func__);
}

int blk_daq_init(void)
{
	my_printk("usb_daq: enter %s\n", __func__);
	if (dev_major_t == 0)
	{
		//ud has initial major=0;
		dev_major_t = register_blkdev(dev_major_t, "blk_daq");
		if (dev_major_t <= 0) {
			my_printk("usb_daq: unable to get blk_daq major number\n");
			return -EBUSY;
		}
		kref_init(&blk_daq_kref);
	}
	my_printk("usb_daq: exit %s\n", __func__);
	return 0;
}

int blk_daq_exit(void)
{
	my_printk("usb_daq: enter %s\n", __func__);
	kref_put(&blk_daq_kref, blk_daq_delete);
	my_printk("usb_daq: exit %s\n", __func__);
	return 0;
}
