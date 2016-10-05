/*
 * usb_daq.h
 *
 *  Created on: 2016å¹?æœ?æ—? *      Author: zhuce
 */

#ifndef USB_DAQ_H_
#define USB_DAQ_H_

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include "my_printk.h"
#include "blk_daq.h"

struct usb_daq_data;

/* Define these values to match your devices */
#define USB_DAQ_VENDOR_ID		0x9408
#define USB_DAQ_PRODUCT_ID		0x2802
#define USB_DAQ_INTF_NUM		0

/* Get a minor range for your devices from the usb maintainer */
#define USB_DAQ_MINOR_BASE	192

/* our private defines. if this grows any larger, use your own .h file */
#define MAX_TRANSFER		(PAGE_SIZE - 512)
/* MAX_TRANSFER is chosen so that the VM is not stressed by
   allocations > PAGE_SIZE and the number of packets in a page
   is an integer 512 is the largest possible packet on EHCI */
#define WRITES_IN_FLIGHT	8
/* arbitrarily chosen */

/* Dynamic bitflag definitions (us->dflags): used in set_bit() etc. */
#define UD_FLIDX_URB_ACTIVE	0	/* current_urb is in use    */
#define UD_FLIDX_SG_ACTIVE	1	/* current_sg is in use     */
#define UD_FLIDX_ABORTING	2	/* abort is in progress     */
#define UD_FLIDX_DISCONNECTING	3	/* disconnect in progress   */
#define UD_FLIDX_RESETTING	4	/* device reset in progress */

/*
 * We provide a DMA-mapped I/O buffer for use with small USB transfers.
 * It turns out that CB[I] needs a 12-byte buffer and Bulk-only needs a
 * 31-byte buffer.  But Freecom needs a 64-byte buffer, so that's the
 * size we'll allocate.
 */
#define UD_IOBUF_SIZE		64	/* Size of the DMA-mapped I/O buffer */


typedef int (*trans_blk_send)(struct usb_daq_data*);
typedef int (*trans_blk_recv)(struct usb_daq_data*);
typedef int (*trans_blk_reset)(struct usb_daq_data*);

/* Structure to hold all of our device specific stuff */
struct usb_daq_data {
	struct usb_device	*pusb_dev;			/* the usb device for this device */
	struct usb_interface	*pusb_intf;		/* the interface for this device */
	u8			ifnum;		 /* interface number   */
	unsigned long		fflags;		 /* fixed flags from filter */
	unsigned long		dflags;		 /* dynamic atomic bitflags */

	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char 	*bulk_in_buffer;	/* the buffer to receive data */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	size_t			bulk_in_copied;		/* already copied to user space */

	unsigned int		send_ctrl_pipe;
	unsigned int		recv_ctrl_pipe;
	unsigned int		send_cmd_bulk_pipe;
	unsigned int		recv_cmd_bulk_pipe;
	unsigned int		send_blk_bulk_pipe;	 /* cached pipe values */
	unsigned int		recv_blk_bulk_pipe;

	trans_blk_send 	blk_send;
	trans_blk_recv 	blk_recv;
	trans_blk_reset blk_reset;

	/* control and bulk communications data */
	struct urb		*current_urb;	 /* USB requests	 */
	struct usb_ctrlrequest	*cr;		 /* control requests	 */
	struct usb_sg_request	current_sg;	 /* scatter-gather req.  */
	unsigned char		*iobuf;		 /* I/O buffer		 */
	dma_addr_t		iobuf_dma;	 /* buffer DMA addresses */
	struct task_struct	*ctl_thread;	 /* the control thread   */

	/* mutual exclusion and synchronization structures */
	struct completion	cmnd_ready;	 /* to sleep thread on	    */

	int				errors;			/* the last request tanked */
	bool			ongoing_read;		/* a read is going on */
	spinlock_t		err_lock;		/* lock for errors */
	struct kref		kref;
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	wait_queue_head_t	bulk_in_wait;		/* to wait for an ongoing read */

	struct blk_daq_dev bd_dev;
};

#define to_usb_daq_dev(d) container_of(d, struct usb_daq_data, kref)


#endif /* USB_DAQ_H_ */
