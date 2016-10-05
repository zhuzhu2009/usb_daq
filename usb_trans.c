/*
 * usb_transport.c
 *
 *  Created on: 2016å¹?æœ?3æ—? *      Author: zhuce
 */

#include <linux/sched.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include <linux/export.h>

#include "usb_daq.h"
#include "usb_transport.h"
#include "my_printk.h"

#include <linux/blkdev.h>


/* This is the completion handler which will wake us up when an URB
 * completes.
 */
static void usb_stor_blocking_completion(struct urb *urb)
{
	struct completion *urb_done_ptr = urb->context;

	complete(urb_done_ptr);
}

/* This is the common part of the URB message submission code
 *
 * All URBs from the usb-storage driver involved in handling a queued scsi
 * command _must_ pass through this function (or something like it) for the
 * abort mechanisms to work properly.
 */
static int usb_daq_msg_common(struct usb_daq_data *ud, int timeout)
{
	struct completion urb_done;
	long timeleft;
	int status;

	/* don't submit URBs during abort processing */
	if (test_bit(UD_FLIDX_ABORTING, &ud->dflags))
		return -EIO;

	/* set up data structures for the wakeup system */
	init_completion(&urb_done);

	/* fill the common fields in the URB */
	ud->current_urb->context = &urb_done;
	ud->current_urb->transfer_flags = 0;

	/* assume that if transfer_buffer isn't us->iobuf then it
	 * hasn't been mapped for DMA.  Yes, this is clunky, but it's
	 * easier than always having the caller tell us whether the
	 * transfer buffer has already been mapped. */
	if (ud->current_urb->transfer_buffer == ud->iobuf)
		ud->current_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	ud->current_urb->transfer_dma = ud->iobuf_dma;

	/* submit the URB */
	status = usb_submit_urb(ud->current_urb, GFP_NOIO);
	if (status) {
		/* something went wrong */
		return status;
	}

	/* since the URB has been submitted successfully, it's now okay
	 * to cancel it */
	set_bit(UD_FLIDX_URB_ACTIVE, &ud->dflags);

	/* did an abort occur during the submission? */
	if (test_bit(UD_FLIDX_ABORTING, &ud->dflags)) {

		/* cancel the URB, if it hasn't been cancelled already */
		if (test_and_clear_bit(UD_FLIDX_URB_ACTIVE, &ud->dflags)) {
			my_printk("usb_daq: cancelling URB\n");
			usb_unlink_urb(ud->current_urb);
		}
	}

	/* wait for the completion of the URB */
	timeleft = wait_for_completion_interruptible_timeout(
			&urb_done, timeout ? : MAX_SCHEDULE_TIMEOUT);

	clear_bit(UD_FLIDX_URB_ACTIVE, &ud->dflags);

	if (timeleft <= 0) {
		my_printk("usb_daq: %s -- cancelling URB\n",
			     timeleft == 0 ? "Timeout" : "Signal");
		usb_kill_urb(ud->current_urb);
	}

	/* return the URB status */
	return ud->current_urb->status;
}

/*
 * Transfer one control message, with timeouts, and allowing early
 * termination.  Return codes are usual -Exxx, *not* USB_STOR_XFER_xxx.
 */
int usb_daq_control_msg(struct usb_daq_data *ud, unsigned int pipe,
		 u8 request, u8 requesttype, u16 value, u16 index,
		 void *data, u16 size, int timeout)
{
	int status;

	my_printk("usb_daq: rq=%02x rqtype=%02x value=%04x index=%02x len=%u\n",
		     request, requesttype, value, index, size);

	/* fill in the devrequest structure */
	ud->cr->bRequestType = requesttype;
	ud->cr->bRequest = request;
	ud->cr->wValue = cpu_to_le16(value);
	ud->cr->wIndex = cpu_to_le16(index);
	ud->cr->wLength = cpu_to_le16(size);

	/* fill and submit the URB */
	usb_fill_control_urb(ud->current_urb, ud->pusb_dev, pipe,
			 (unsigned char*) ud->cr, data, size,
			 usb_stor_blocking_completion, NULL);
	status = usb_daq_msg_common(ud, timeout);

	/* return the actual length of the data transferred if no error */
	if (status == 0)
		status = ud->current_urb->actual_length;
	return status;
}
EXPORT_SYMBOL_GPL(usb_daq_control_msg);

/* Determine what the maximum LUN supported is */
int usb_daq_get_dev_info(struct usb_daq_data *ud)
{
	int result;

	/* issue the command */
	memset(ud->iobuf, 0, 64);
	result = usb_daq_control_msg(ud, ud->recv_ctrl_pipe,
				 UD_GET_DEV_INFO,
				 USB_DIR_IN | USB_TYPE_VENDOR |
				 USB_RECIP_INTERFACE,
				 0, ud->ifnum, ud->iobuf, 12, 10*HZ);

	/*
	 * If we have a successful request, return the result if valid. The
	 * info field is 8 bytes wide, so the value reported by the device
	 * should fit into that.
	 */
	if (result >= 12) {
		ud->bd_dev.hardsect_size = *(int*)(ud->iobuf);
		ud->bd_dev.nsectors = *(u64*)(ud->iobuf + 4);

		my_printk("usb_daq: usb_daq_get_dev_info, sect size %d, nsect %llu",
				ud->bd_dev.hardsect_size, ud->bd_dev.nsectors);
	}
	else
	{
		my_printk("usb_daq: usb_daq_get_dev_info result < 12 is %d\n",
			     result);
	}


	return 0;
}
