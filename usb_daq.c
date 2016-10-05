/*
 * usb_daq_data.c
 *
 *  Created on: 2015-9-8
 *      Author: zhuce
 */

/*
 * 生成驱动模块后使用insmod xxx.ko就可以插入到内核中运行了ﺿ * 用lsmod可以看到你插入到内核中的模块ﺿ * 也可以从系统中用命令rmmod xxx把模块卸载掉.
 * 如果把编译出来的驱动模块拷贝冿lib/modules/~/kernel/drivers/usb/下，
 * 然后depmod一下， 那么你在插入USB设备的时候，系统就会自动为你加载驱动模块瘿
 * 当然这个得有hotplug的支抿
 * 加载驱动模块成功后就会在/dev/下生成设备文件了ﺿ * 先挂载usbfs：mount -t usbfs none /proc/bus/usb
 * 如果用命令cat /proc/bus/usb/devices，我们可以看到驱动程序已经绑定到接口上了.
 */

#include "usb_daq.h"
#include "usb_trans.h"
#include <linux/kthread.h>

/* table of devices that work with this driver */
static const struct usb_device_id usb_daq_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(USB_DAQ_VENDOR_ID, USB_DAQ_PRODUCT_ID, USB_DAQ_INTF_NUM) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, usb_daq_table);

//#define to_usb_daq_dev(d) container_of(d, struct usb_daq_data, kref)

static struct usb_driver usb_daq_driver;
static void usb_daq_draw_down(struct usb_daq_data *ud);
static void usb_daq_delete(struct kref *kref);

static int usb_daq_open(struct inode *inode, struct file *file)
{
	struct usb_daq_data *ud;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	my_printk("usb_daq: enter usb_daq_open\n");

	interface = usb_find_interface(&usb_daq_driver, subminor);
	if (!interface) {
		pr_err("usb_daq: %s - error, can't find device for minor %d\n",
			__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	ud = usb_get_intfdata(interface);
	if (!ud) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(interface);
	if (retval)
		goto exit;

	/* increment our usage count for the device */
	kref_get(&ud->kref);

	/* save our object in the file's private structure */
	file->private_data = ud;

	my_printk("usb_daq: exit usb_daq_open\n");

exit:
	return retval;
}

static int usb_daq_release(struct inode *inode, struct file *file)
{
	struct usb_daq_data *ud;

	ud = file->private_data;
	if (ud == NULL)
		return -ENODEV;

	my_printk("usb_daq: enter %s\n", __func__);

	/* allow the device to be autosuspended */
	mutex_lock(&ud->io_mutex);
	if (ud->pusb_intf)
		usb_autopm_put_interface(ud->pusb_intf);
	mutex_unlock(&ud->io_mutex);

	/* decrement the count on our device */
	kref_put(&ud->kref, usb_daq_delete);

	my_printk("usb_daq: exit %s\n", __func__);

	return 0;
}

static int usb_daq_flush(struct file *file, fl_owner_t id)
{
	struct usb_daq_data *ud;
	int res;

	ud = file->private_data;
	if (ud == NULL)
		return -ENODEV;

	my_printk("usb_daq: enter %s\n", __func__);

	/* wait for io to stop */
	mutex_lock(&ud->io_mutex);
	usb_daq_draw_down(ud);

	/* read out errors, leave subsequent opens a clean slate */
	spin_lock_irq(&ud->err_lock);
	res = ud->errors ? (ud->errors == -EPIPE ? -EPIPE : -EIO) : 0;
	ud->errors = 0;
	spin_unlock_irq(&ud->err_lock);

	mutex_unlock(&ud->io_mutex);

	my_printk("usb_daq: exit %s\n", __func__);

	return res;
}

static void usb_daq_read_bulk_callback(struct urb *urb)
{
	struct usb_daq_data *ud;

	ud = urb->context;

	my_printk("usb_daq: enter %s\n", __func__);

	spin_lock(&ud->err_lock);
	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&ud->pusb_intf->dev,
				"helloUSB: %s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		ud->errors = urb->status;
	} else {
		ud->bulk_in_filled = urb->actual_length;
	}
	ud->ongoing_read = 0;
	spin_unlock(&ud->err_lock);

	wake_up_interruptible(&ud->bulk_in_wait);

	my_printk("usb_daq: exit %s\n", __func__);
}

static int usb_daq_do_read_io(struct usb_daq_data *ud, size_t count)
{
	int rv;

	my_printk("usb_daq: enter %s\n", __func__);

	/* prepare a read */
	usb_fill_bulk_urb(ud->bulk_in_urb,
			ud->pusb_dev,
			ud->recv_cmd_bulk_pipe,
			ud->bulk_in_buffer,
			min(ud->bulk_in_size, count),
			usb_daq_read_bulk_callback,
			ud);
	/* tell everybody to leave the URB alone */
	spin_lock_irq(&ud->err_lock);
	ud->ongoing_read = 1;
	spin_unlock_irq(&ud->err_lock);

	/* submit bulk in urb, which means no data to deliver */
	ud->bulk_in_filled = 0;
	ud->bulk_in_copied = 0;

	/* do it */
	rv = usb_submit_urb(ud->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		dev_err(&ud->pusb_intf->dev,
			"usb_daq: %s - failed submitting read urb, error %d\n",
			__func__, rv);
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irq(&ud->err_lock);
		ud->ongoing_read = 0;
		spin_unlock_irq(&ud->err_lock);
	}

	my_printk("usb_daq: exit %s\n", __func__);

	return rv;
}

static ssize_t usb_daq_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos)
{
	struct usb_daq_data *ud;
	int rv;
	bool ongoing_io;

	ud = file->private_data;

	my_printk("usb_daq: enter %s, count: %d\n", __func__, count);

	/* if we cannot read at all, return EOF */
	if (!ud->bulk_in_urb || !count)
		return 0;

	/* no concurrent readers */
	rv = mutex_lock_interruptible(&ud->io_mutex);
	if (rv < 0)
		return rv;

	if (!ud->pusb_intf) {		/* disconnect() was called */
		rv = -ENODEV;
		goto exit;
	}


	/* if IO is under way, we must not touch things */
retry:
	spin_lock_irq(&ud->err_lock);
	ongoing_io = ud->ongoing_read;
	spin_unlock_irq(&ud->err_lock);

	if (ongoing_io) {
		/* nonblocking IO shall not wait */
		if (file->f_flags & O_NONBLOCK) {
			rv = -EAGAIN;
			goto exit;
		}
		/*
		 * IO may take forever
		 * hence wait in an interruptible state
		 */
		rv = wait_event_interruptible(ud->bulk_in_wait, (!ud->ongoing_read));
		if (rv < 0)
			goto exit;
	}

	/* errors must be reported */
	rv = ud->errors;
	if (rv < 0) {
		/* any error is reported once */
		ud->errors = 0;
		/* to preserve notifications about reset */
		rv = (rv == -EPIPE) ? rv : -EIO;
		/* report it */
		goto exit;
	}

	/*
	 * if the buffer is filled we may satisfy the read
	 * else we need to start IO
	 */

	if (ud->bulk_in_filled) {
		/* we had read data */
		size_t available = ud->bulk_in_filled - ud->bulk_in_copied;
		size_t chunk = min(available, count);

		if (!available) {
			/*
			 * all data has been used
			 * actual IO needs to be done
			 */
			rv = usb_daq_do_read_io(ud, count);
			if (rv < 0)
				goto exit;
			else
				goto retry;
		}
		/*
		 * data is available
		 * chunk tells us how much shall be copied
		 */

		if (copy_to_user(buffer,
				 ud->bulk_in_buffer + ud->bulk_in_copied,
				 chunk))
			rv = -EFAULT;
		else
			rv = chunk;

		ud->bulk_in_copied += chunk;

		/*
		 * if we are asked for more than we have,
		 * we start IO but don't wait
		 */
		if (available < count)
			usb_daq_do_read_io(ud, count - chunk);
	} else {
		/* no data in the buffer */

		rv = usb_daq_do_read_io(ud, count);
		if (rv < 0)
			goto exit;
		else
			goto retry;
	}
exit:
	mutex_unlock(&ud->io_mutex);

	my_printk("helloUSB: exit %s\n", __func__);

	return rv;
}

static void usb_daq_write_bulk_callback(struct urb *urb)
{
	struct usb_daq_data *ud;

	ud = urb->context;

	my_printk("helloUSB: enter %s\n", __func__);

	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&ud->pusb_intf->dev,
				"usb_daq: %s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		spin_lock(&ud->err_lock);
		ud->errors = urb->status;
		spin_unlock(&ud->err_lock);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);
	up(&ud->limit_sem);

	my_printk("usb_daq: exit %s\n", __func__);
}

static ssize_t usb_daq_write(struct file *file, const char *user_buffer,
			  size_t count, loff_t *ppos)
{
	struct usb_daq_data *ud;
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;
	size_t writesize = min(count, (size_t)MAX_TRANSFER);

	ud = file->private_data;

	my_printk("usb_daq: enter %s\n", __func__);

	/* verify that we actually have some data to write */
	if (count == 0)
		goto exit;

	/*
	 * limit the number of URBs in flight to stop a user from using up all
	 * RAM
	 */
	if (!(file->f_flags & O_NONBLOCK)) {
		if (down_interruptible(&ud->limit_sem)) {
			retval = -ERESTARTSYS;
			goto exit;
		}
	} else {
		if (down_trylock(&ud->limit_sem)) {
			retval = -EAGAIN;
			goto exit;
		}
	}

	spin_lock_irq(&ud->err_lock);
	retval = ud->errors;
	if (retval < 0) {
		/* any error is reported once */
		ud->errors = 0;
		/* to preserve notifications about reset */
		retval = (retval == -EPIPE) ? retval : -EIO;
	}
	spin_unlock_irq(&ud->err_lock);
	if (retval < 0)
		goto error;

	/* create a urb, and a buffer for it, and copy the data to the urb */
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(ud->pusb_dev, writesize, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf) {
		retval = -ENOMEM;
		goto error;
	}

	if (copy_from_user(buf, user_buffer, writesize)) {
		retval = -EFAULT;
		goto error;
	}

	/* this lock makes sure we don't submit URBs to gone devices */
	mutex_lock(&ud->io_mutex);
	if (!ud->pusb_intf) {		/* disconnect() was called */
		mutex_unlock(&ud->io_mutex);
		retval = -ENODEV;
		goto error;
	}

	/* initialize the urb properly */
	usb_fill_bulk_urb(urb, ud->pusb_dev, ud->send_cmd_bulk_pipe,
			  buf, writesize, usb_daq_write_bulk_callback, ud);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &ud->submitted);

	/* send the data out the bulk port */
	retval = usb_submit_urb(urb, GFP_KERNEL);
	mutex_unlock(&ud->io_mutex);
	if (retval) {
		dev_err(&ud->pusb_intf->dev,
			"usb_daq: %s - failed submitting write urb, error %d\n",
			__func__, retval);
		goto error_unanchor;
	}

	/*
	 * release our reference to this urb, the USB core will eventually free
	 * it entirely
	 */
	usb_free_urb(urb);


	my_printk("usb_daq: exit %s\n", __func__);

	return writesize;

error_unanchor:
	usb_unanchor_urb(urb);
error:
	if (urb) {
		usb_free_coherent(ud->pusb_dev, writesize, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}
	up(&ud->limit_sem);

exit:
	return retval;
}

static long usb_daq_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct usb_daq_data *ud;
	int rv;
	unsigned long *pUser = (unsigned long *)arg;

	ud = file->private_data;

	my_printk("usb_daq: enter %s\n", __func__);

	my_printk("usb_daq: blk_daq_ioctl cmd %#x, arg %#lx\n", cmd, arg);
	switch (cmd) {
		case IOCTL_BLK_DAQ_START_WRITE:
			atomic_inc(&(ud->bd_dev.aWrite));
			get_user(ud->bd_dev.ex_wr_size, pUser);
			my_printk("usb_daq: blk_daq_ioctl inc %d, ex_wr_size %d\n",
					atomic_read(&(ud->bd_dev.aWrite)), ud->bd_dev.ex_wr_size);
			break;
		case IOCTL_BLK_DAQ_STOP_WRITE:
			//atomic_dec(&(dev->aWrite));
			if (!atomic_dec_and_test(&(ud->bd_dev.aWrite)))
			{
				return -ENOIOCTLCMD;
			}
			my_printk("usb_daq: blk_daq_ioctl dec %d\n", atomic_read(&(ud->bd_dev.aWrite)));
			break;
		default:
			return -ENOIOCTLCMD;
			break;
	}

	my_printk("usb_daq: exit %s\n", __func__);
}

static const struct file_operations usb_daq_fops = {
	.owner =	THIS_MODULE,
	.read =		usb_daq_read,
	.write =	usb_daq_write,
	.unlocked_ioctl = usb_daq_ioctl,
	.open =		usb_daq_open,
	.release =	usb_daq_release,
	.flush =	usb_daq_flush,
	.llseek =	noop_llseek,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver usb_daq_class = {
	.name =		"usb_daq%d",
	.fops =		&usb_daq_fops,
	.minor_base =	USB_DAQ_MINOR_BASE,
};

/***********************************************************************
 * Device probing and disconnecting
 ***********************************************************************/

/* Associate our private data with the USB device */
static int associate_dev(struct usb_daq_data *ud, struct usb_interface *intf)
{
	/* Fill in the device-related fields */
	ud->pusb_dev = usb_get_dev(interface_to_usbdev(intf));//interface_to_usbdev(intf);
	ud->pusb_intf = intf;
	ud->ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	my_printk("usb_daq: Vendor: 0x%04x, Product: 0x%04x, Revision: 0x%04x\n",
		     le16_to_cpu(ud->pusb_dev->descriptor.idVendor),
		     le16_to_cpu(ud->pusb_dev->descriptor.idProduct),
		     le16_to_cpu(ud->pusb_dev->descriptor.bcdDevice));
	my_printk("usb_daq: Interface Subclass: 0x%02x, Protocol: 0x%02x\n",
		     intf->cur_altsetting->desc.bInterfaceSubClass,
		     intf->cur_altsetting->desc.bInterfaceProtocol);

	/* Store our private data in the interface */
	usb_set_intfdata(intf, ud);

	/* Allocate the control/setup and DMA-mapped buffers */
	ud->cr = kmalloc(sizeof(*ud->cr), GFP_KERNEL);
	if (!ud->cr)
		return -ENOMEM;

	ud->iobuf = usb_alloc_coherent(ud->pusb_dev, UD_IOBUF_SIZE,
			GFP_KERNEL, &ud->iobuf_dma);

	if (!ud->iobuf) {
		my_printk("usb_daq: I/O buffer allocation failed\n");
		return -ENOMEM;
	}

	return 0;
}

/* Get the pipe settings */
static int get_pipes(struct usb_daq_data *ud)
{
	struct usb_host_interface *altsetting = ud->pusb_intf->cur_altsetting;
	int i;
	struct usb_endpoint_descriptor *ep;
	struct usb_endpoint_descriptor *ep_cmd_in = NULL;
	struct usb_endpoint_descriptor *ep_cmd_out = NULL;
	struct usb_endpoint_descriptor *ep_blk_in = NULL;
	struct usb_endpoint_descriptor *ep_blk_out = NULL;

	/*
	 * Find the first endpoint of each type we need.
	 * We are expecting a minimum of 2 endpoints - in and out (bulk).
	 * An optional interrupt-in is OK (necessary for CBI protocol).
	 * We will ignore any others.
	 */
	for (i = 0; i < altsetting->desc.bNumEndpoints; i++) {
		ep = &altsetting->endpoint[i].desc;
		my_printk("usb_daq: find endpoint 0x%02x.\n", ep->bEndpointAddress);
		if (ep->bEndpointAddress == 0x81) {
			ep_cmd_in = ep;
		}
		if (ep->bEndpointAddress == 0x01) {
			ep_cmd_out = ep;
		}
		if (ep->bEndpointAddress == 0x86) {
			ep_blk_in = ep;
		}
		if (ep->bEndpointAddress == 0x02) {
			ep_blk_out = ep;
		}
	}

	if (!ep_cmd_in || !ep_cmd_out || !ep_blk_in || !ep_blk_out) {
		my_printk("usb_daq: endpoint sanity check failed! Rejecting dev.\n");
		return -EIO;
	}

	/* Calculate and store the pipe values */
	ud->send_ctrl_pipe = usb_sndctrlpipe(ud->pusb_dev, 0);
	ud->recv_ctrl_pipe = usb_rcvctrlpipe(ud->pusb_dev, 0);
	ud->send_cmd_bulk_pipe = usb_sndbulkpipe(ud->pusb_dev, usb_endpoint_num(ep_cmd_out));
	ud->recv_cmd_bulk_pipe = usb_rcvbulkpipe(ud->pusb_dev, usb_endpoint_num(ep_cmd_in));
	ud->send_blk_bulk_pipe = usb_sndbulkpipe(ud->pusb_dev, usb_endpoint_num(ep_blk_out));
	ud->recv_blk_bulk_pipe = usb_rcvbulkpipe(ud->pusb_dev, usb_endpoint_num(ep_blk_in));

	return 0;
}

/* Initialize all the dynamic resources we need */
static int usb_stor_acquire_resources(struct usb_daq_data *ud)
{
	int p;
	struct task_struct *th;

	ud->bulk_in_size = usb_maxpacket(ud->pusb_dev, ud->recv_cmd_bulk_pipe, usb_pipeout(ud->recv_cmd_bulk_pipe));//usb_endpoint_maxp(ep_cmd_in);
	my_printk("usb_daq: bulk_in_size %d\n", ud->bulk_in_size);

	ud->bulk_in_buffer = kmalloc(ud->bulk_in_size, GFP_KERNEL);
	if (!ud->bulk_in_buffer) {
		dev_err(&ud->pusb_intf->dev, "usb_daq: Could not allocate bulk_in_buffer\n");
		return 0;
	}
	ud->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ud->bulk_in_urb) {
		dev_err(&ud->pusb_intf->dev, "usb_daq: Could not allocate bulk_in_urb\n");
		return 0;
	}

	ud->current_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ud->current_urb) {
		my_printk("usb_daq: URB allocation failed\n");
		return -ENOMEM;
	}

	/* Start up our control thread */
	ud->ctl_thread = NULL;
//	th = kthread_run(usb_stor_control_thread, us, "usb-storage");
//	if (IS_ERR(th)) {
//		dev_warn(&ud->pusb_intf->dev,
//				"usb_daq: Unable to start control thread\n");
//		return PTR_ERR(th);
//	}
//	ud->ctl_thread = th;

	return 0;
}

/* Release all our dynamic resources */
static void usb_stor_release_resources(struct usb_daq_data *ud)
{
	my_printk("usb_daq: enter %s\n", __func__);

	usb_free_urb(ud->bulk_in_urb);
	kfree(ud->bulk_in_buffer);
	/* Tell the control thread to exit.  The host must
	 * already have been removed and the DISCONNECTING flag set
	 * so that we won't accept any more commands.
	 */
	my_printk("usb_daq: -- sending exit command to thread\n");
	complete(&ud->cmnd_ready);
	if (ud->ctl_thread)
		kthread_stop(ud->ctl_thread);

	usb_free_urb(ud->current_urb);

	my_printk("usb_daq: exit %s\n", __func__);
}

/* Dissociate from the USB device */
static void dissociate_dev(struct usb_daq_data *ud)
{
	my_printk("usb_daq: enter %s\n", __func__);
	/* Free the buffers */
	kfree(ud->cr);
	usb_free_coherent(ud->pusb_dev, UD_IOBUF_SIZE, ud->iobuf, ud->iobuf_dma);

	/* Remove our private data from the interface */
	usb_set_intfdata(ud->pusb_intf, NULL);

	my_printk("usb_daq: exit %s\n", __func__);
}

/* Second stage of disconnect processing: deallocate all resources */
static void release_everything(struct usb_daq_data *ud)
{
	usb_stor_release_resources(ud);
	dissociate_dev(ud);
}

static void usb_daq_delete(struct kref *kref)
{
	struct usb_daq_data *ud = to_usb_daq_dev(kref);

	my_printk("usb_daq: enter usb_daq_delete\n");

	release_everything(ud);

	usb_put_dev(ud->pusb_dev);
	kfree(ud);

	my_printk("usb_daq: exit usb_daq_delete\n");
}

static int usb_daq_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_daq_data *ud;
	int retval = -ENOMEM;

	my_printk("usb_daq: enter %s\n", __func__);

	/* allocate memory for our device state and initialize it */
	ud = kzalloc(sizeof(*ud), GFP_KERNEL);
	if (!ud) {
		dev_err(&interface->dev, "usb_daq: Out of memory\n");
		goto error;
	}
	kref_init(&ud->kref);
	sema_init(&ud->limit_sem, WRITES_IN_FLIGHT);
	mutex_init(&ud->io_mutex);
	spin_lock_init(&ud->err_lock);
	init_usb_anchor(&ud->submitted);
	init_waitqueue_head(&ud->bulk_in_wait);

	init_completion(&ud->cmnd_ready);

	retval = associate_dev(ud, interface);
	if (retval)
		goto error;

	/* set up the endpoint information */
	retval = get_pipes(ud);
	if (retval)
		goto error;

	/* save our data pointer in this interface device */
	//do in associate_dev()
	//usb_set_intfdata(interface, ud);

	/* we can register the device now, as it is ready */
	retval = usb_register_dev(interface, &usb_daq_class);
	if (retval) {
		/* something prevented us from registering this driver */
		dev_err(&interface->dev, "usb_daq: Not able to get a minor for this device.\n");
		//usb_set_intfdata(interface, NULL);
		goto error;
	}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "usb_daq: usb daq device now attached to usb_daq%d",
		 interface->minor);

	retval = usb_stor_acquire_resources(ud);
	if (retval)
		goto error;

	retval = usb_daq_get_dev_info(ud);
	if (retval) {
		my_printk("usb_daq: usb_daq_get_dev_info error\n");
		goto error;
	}

	retval = blk_daq_init();
	if (retval) {
		my_printk("usb_daq: blk_daq_init error\n");
		goto error;
	}

	retval = blk_daq_add_device(&(ud->bd_dev));
	if (retval) {
		my_printk("usb_daq: blk_daq_add_device error\n");
		goto error;
	}

	my_printk("usb_daq: exit %s\n", __func__);

	return 0;

error:
	my_printk("usb_daq: exit %s error\n", __func__);
	if (ud)
		/* this frees allocated memory */
		kref_put(&ud->kref, usb_daq_delete);
	return retval;
}

static void usb_daq_disconnect(struct usb_interface *interface)
{
	int retval = 0;
	struct usb_daq_data *ud;
	int minor = interface->minor;
	ud = usb_get_intfdata(interface);

	my_printk("usb_daq: enter %s\n", __func__);

	retval = blk_daq_del_device(&(ud->bd_dev));
	if (retval) {
		my_printk("usb_daq: blk_daq_del_device error\n");
	}

	retval = blk_daq_exit();
	if (retval) {
		my_printk("usb_daq: blk_daq_exit error\n");
	}

	//scsi_lock(host);
	set_bit(UD_FLIDX_DISCONNECTING, &ud->dflags);
	//scsi_unlock(host);

	//do in release_everything
	//usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &usb_daq_class);

	/* prevent more I/O from starting */
//	mutex_lock(&ud->io_mutex);
//	ud->pusb_intf = NULL;
//	mutex_unlock(&ud->io_mutex);

	usb_kill_anchored_urbs(&ud->submitted);

	/* decrement our usage count */
	kref_put(&ud->kref, usb_daq_delete);

	dev_info(&interface->dev, "usb_daq: usb_daq #%d now disconnected", minor);

	my_printk("usb_daq: exit %s\n", __func__);
}

static void usb_daq_draw_down(struct usb_daq_data *ud)
{
	int time;

	my_printk("usb_daq: enter %s\n", __func__);

	time = usb_wait_anchor_empty_timeout(&ud->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&ud->submitted);
	usb_kill_urb(ud->bulk_in_urb);

	my_printk("usb_daq: exit %s\n", __func__);
}

static int usb_daq_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_daq_data *ud = usb_get_intfdata(intf);

	my_printk("usb_daq: enter %s\n", __func__);

	if (!ud)
		return 0;
	usb_daq_draw_down(ud);

	my_printk("usb_daq: exit %s\n", __func__);

	return 0;
}

static int usb_daq_resume(struct usb_interface *intf)
{
	my_printk("usb_daq: enter %s\n", __func__);

	my_printk("usb_daq: exit %s\n", __func__);

	return 0;
}

static int usb_daq_pre_reset(struct usb_interface *intf)
{
	struct usb_daq_data *ud = usb_get_intfdata(intf);

	my_printk("usb_daq: enter %s\n", __func__);

	mutex_lock(&ud->io_mutex);
	usb_daq_draw_down(ud);

	my_printk("usb_daq: exit %s\n", __func__);

	return 0;
}

static int usb_daq_post_reset(struct usb_interface *intf)
{
	struct usb_daq_data *ud = usb_get_intfdata(intf);

	my_printk("usb_daq: enter %s\n", __func__);

	/* we are sure no URBs are active - no locking needed */
	ud->errors = -EPIPE;
	mutex_unlock(&ud->io_mutex);

	my_printk("usb_daq: exit %s\n", __func__);

	return 0;
}

static struct usb_driver usb_daq_driver = {
	.name =		"usb_daq",
	.probe =	usb_daq_probe,
	.disconnect =	usb_daq_disconnect,
	.suspend =	usb_daq_suspend,
	.resume =	usb_daq_resume,
	.pre_reset =	usb_daq_pre_reset,
	.post_reset =	usb_daq_post_reset,
	.id_table =	usb_daq_table,
	.supports_autosuspend = 1,
};

module_usb_driver(usb_daq_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhuce");
