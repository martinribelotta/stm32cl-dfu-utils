#ifndef _DFUSE_H
#define _DFUSE_H

int dfuse_do_upload(struct usb_dev_handle *usb_handle, int interface, 
		      int xfer_size, const char *fname);
int dfuse_do_bin_dnload(struct usb_dev_handle *usb_handle, int interface,
		      int xfer_size, const char *fname, int address);
int dfuse_do_raw_dnload(struct usb_dev_handle *usb_handle, int interface,
		      int xfer_size, const char *fname, int address);
int dfuse_do_dfuse_dnload(struct usb_dev_handle *usb_handle, int interface,
		      int xfer_size, const char *fname);
int dfuse_set_address_pointer(struct usb_dev_handle *usb_handle, int interface,
                      unsigned int address, int erase);

#endif
