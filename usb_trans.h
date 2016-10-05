/*
 * usb_trans.h
 *
 *  Created on: 2016�?�?3�? *      Author: zhuce
 */

#ifndef USB_TRANS_H_
#define USB_TRANS_H_

#include "usb_daq.h"

#define UD_GET_DEV_INFO		0xfe

/*
 * usb_stor_bulk_transfer_xxx() return codes, in order of severity
 */

#define USB_DAQ_XFER_GOOD	0	/* good transfer                 */
#define USB_DAQ_XFER_SHORT	1	/* transferred less than expected */
#define USB_DAQ_XFER_STALLED	2	/* endpoint stalled              */
#define USB_DAQ_XFER_LONG	3	/* device tried to send too much */
#define USB_DAQ_XFER_ERROR	4	/* transfer died in the middle   */

/*
 * Transport return codes
 */

#define USB_DAQ_TRANSPORT_GOOD	   0   /* Transport good, command good	   */
#define USB_DAQ_TRANSPORT_FAILED  1   /* Transport good, command failed   */
#define USB_DAQ_TRANSPORT_NO_SENSE 2  /* Command failed, no auto-sense    */
#define USB_DAQ_TRANSPORT_ERROR   3   /* Transport bad (i.e. device dead) */

/*
 * We used to have USB_DAQ_XFER_ABORTED and USB_DAQ_TRANSPORT_ABORTED
 * return codes.  But now the transport and low-level transfer routines
 * treat an abort as just another error (-ENOENT for a cancelled URB).
 * It is up to the invoke_transport() function to test for aborts and
 * distinguish them from genuine communication errors.
 */


extern int usb_daq_control_msg(struct usb_daq_data *ud, unsigned int pipe,
		u8 request, u8 requesttype, u16 value, u16 index,
		void *data, u16 size, int timeout);
extern int usb_daq_clear_halt(struct usb_daq_data *ud, unsigned int pipe);
extern int usb_daq_bulk_transfer_buf(struct usb_daq_data *ud, unsigned int pipe,
		void *buf, unsigned int length, unsigned int *act_len);
extern int usb_daq_get_dev_info(struct usb_daq_data *ud);


#endif /* USB_TRANSPORT_H_ */
