/* This implements part of the ST DfuSe 1.1a specification
 * which is different from the USB DFU 1.0 and 1.1 specs.
 *
 * (C) 2007-2008 by Harald Welte <laforge@gnumonks.org>
 * (C) 2010 Tormod Volden <debian.tormod@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <usb.h>

#include "config.h"
#include "dfu.h"
#include "usb_dfu.h"
#include "dfuse.h"

/* ugly hack for Win32 */
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define ERASE 1

int last_erased = 0;
unsigned short transaction;

unsigned int quad(char *p) {
    return ((unsigned char)*p + ((unsigned char)*(p+1) << 8) +
	   ((unsigned char)*(p+2) << 16) + ((unsigned char)*(p+3) << 24));
}

void dfuse_init()
{
    dfu_debug( debug );
    dfu_init( 5000 );
}

/* Either set address pointer or erase page at given address */
int dfuse_set_address_pointer(struct usb_dev_handle *usb_handle, int interface,
		      unsigned int address, int command)
{
    char buf[5];
    int ret;
    struct dfu_status dst;

    if (command == ERASE) {
	printf(" Erasing page at address 0x%08x, 1k page starting at 0x%08x\n",
		address, address & (~1023));
	buf[0] = 0x41; /* Erase command */
	last_erased = address;
    } else {
	printf(" Setting address pointer to 0x%08x\n", address);
	buf[0] = 0x21; /* Set Address Pointer command */
    }
    buf[1] = address & 0xff;
    buf[2] = (address >> 8) & 0xff;
    buf[3] = (address >> 16) & 0xff;
    buf[4] = (address >> 24) & 0xff;
	
#ifdef DEBUG_DRY
    return 0;
#endif
    ret = dfu_download(usb_handle, interface, 5, buf, 0);
    if (ret < 0) {
	fprintf(stderr, "Error during special command download\n");
	exit(1);
    }
    ret = dfu_get_status(usb_handle, interface, &dst);
    if (ret < 0) {
	fprintf(stderr, "Error during get_status\n");
	exit(1);
    }
    if (dst.bState != DFU_STATE_dfuDNBUSY) {
	fprintf(stderr, "Error has occured\n");
	exit(1);
    }
    /* wait while command is executed */
    usleep(dst.bwPollTimeout * 1000);

    ret = dfu_get_status(usb_handle, interface, &dst);
    if (ret < 0) {
	fprintf(stderr, "Error during second get_status\n");
	exit(1);
    }
    if (dst.bStatus != DFU_STATUS_OK) {
	fprintf(stderr, "Error: Command not correctly executed\n");
	exit(1);
    }
    usleep(dst.bwPollTimeout * 1000);

    ret = dfu_abort(usb_handle, interface);
    if (ret < 0) {
	fprintf(stderr, "Error sending dfu abort request\n");
	exit(1);
    }
    ret = dfu_get_status(usb_handle, interface, &dst);
    if (ret < 0) {
	fprintf(stderr, "Error during abort get_status\n");
	exit(1);
    }
    if (dst.bState != DFU_STATE_dfuIDLE) {
	fprintf(stderr, "Failed to enter idle state on abort\n");
	exit(1);
    }
    usleep(dst.bwPollTimeout * 1000);
    return ret;
}

int dfuse_do_upload(struct usb_dev_handle *usb_handle, int interface,
		      int xfer_size, const char *fname)
{
	int ret, fd, total_bytes = 0;
	char *buf;

	buf = malloc(xfer_size);
	if (!buf)
		return -ENOMEM;

	fd = creat(fname, 0644);
	if (fd < 0) {
		perror(fname);
		ret = fd;
		goto out_free;
	}

	printf("bytes_per_hash=%u\n", xfer_size);
	printf("Starting upload: [");
	fflush(stdout);

	transaction = 2;
	while (1) {
		int rc, write_rc;
		rc = dfu_upload(usb_handle, interface, xfer_size, buf, transaction++);
		if (rc < 0) {
			ret = rc;
			goto out_close;
		}
		write_rc = write(fd, buf, rc);
		if (write_rc < rc) {
			fprintf(stderr, "Short file write: %s\n",
				strerror(errno));
			ret = -1;
			goto out_close;
		}
		total_bytes += rc;
		/* FIXME: upload a requested size */
		/* or figure out size from descriptor strings */
		if (rc < xfer_size || total_bytes >= 0x8000) {
			/* last block, return successfully */
			ret = total_bytes;
			break;
		}
		putchar('#');
		fflush(stdout);
	}

	printf("] finished!\n");
	fflush(stdout);

out_close:
	close(fd);
out_free:
	free(buf);

	return ret;
}

#define PROGRESS_BAR_WIDTH 50

int dfuse_dnload_chunk(struct usb_dev_handle *usb_handle, int interface,
			char *data, int size, int transaction)
{
	int bytes_sent;
	struct dfu_status dst;
	int ret;

	ret = dfu_download(usb_handle, interface, size, size ? data : NULL,
				transaction);
	if (ret < 0) {
		fprintf(stderr, "Error during download\n");
		return ret;
	}
	bytes_sent = ret;

	do {
		ret = dfu_get_status(usb_handle, interface, &dst);
		if (ret < 0) {
			fprintf(stderr, "Error during download get_status\n");
			return ret;
		}
		usleep(dst.bwPollTimeout * 1000);
	} while (dst.bState != DFU_STATE_dfuDNLOAD_IDLE &&
		 dst.bState != DFU_STATE_dfuERROR);

	if (dst.bStatus != DFU_STATUS_OK) {
		printf(" failed!\n");
		printf("state(%u) = %s, status(%u) = %s\n", dst.bState,
			dfu_state_to_string(dst.bState), dst.bStatus,
			dfu_status_to_string(dst.bStatus));
		return -1;
	}
	return bytes_sent;
}

/* This is not working yet! */
/* Download non-DfuSe file to DfuSe device at requested address */
int dfuse_do_raw_dnload(struct usb_dev_handle *usb_handle, int interface,
		      int xfer_size, const char *fname, int address)
{
	int ret, fd, bytes_sent = 0;
	unsigned int bytes_per_hash, hashes = 0;
	char *buf;
	struct stat st;
	struct dfu_status dst;

	buf = malloc(xfer_size);
	if (!buf)
		return -ENOMEM;

	fd = open(fname, O_RDONLY|O_BINARY);
	if (fd < 0) {
		perror(fname);
		ret = fd;
		goto out_free;
	}

	ret = fstat(fd, &st);
	if (ret < 0) {
		perror(fname);
		goto out_close;
	}

	if (st.st_size <= 0) {
		fprintf(stderr, "File seems a bit too small...\n");
		ret = -EINVAL;
		goto out_close;	
	}

	dfuse_set_address_pointer(usb_handle, interface, address, 0);

	fprintf(stderr, "Raw downloading not implemented yet\n");
	exit(0);

	bytes_per_hash = st.st_size / PROGRESS_BAR_WIDTH;
	if (bytes_per_hash == 0)
		bytes_per_hash = 1;
	printf("bytes_per_hash=%u\n", bytes_per_hash);

	printf("Starting download: [");
	fflush(stdout);
	transaction = 2;
	while (bytes_sent < st.st_size /* - DFU_HDR */) {
		int hashes_todo;
		int chunk_size;

		chunk_size = read(fd, buf, xfer_size);
		if (chunk_size < 0) {
			perror(fname);
			goto out_close;
		}

		/* FIXME: must keep track of address and erase page first */
		ret = dfuse_dnload_chunk(usb_handle, interface,
				buf, chunk_size, transaction++);
		if (ret != chunk_size) {
			fprintf(stderr, "Failed to download chunk %i"
				"of size %i\n", transaction - 2, chunk_size);
			goto out_close;
		}
		bytes_sent += ret;

		hashes_todo = (bytes_sent / bytes_per_hash) - hashes;
		hashes += hashes_todo;
		while (hashes_todo--)
			putchar('#');
		fflush(stdout);
	}

	/* send one zero sized download request to signalize end */
	ret = dfu_download(usb_handle, interface, 0, NULL, transaction++);
	if (ret < 0) {
		fprintf(stderr, "Error sending completion packet\n");
	}
	printf("] finished!\n");
	fflush(stdout);

get_status:
	/* Transition to MANIFEST_SYNC state */
	ret = dfu_get_status(usb_handle, interface, &dst);
	if (ret < 0) {
		fprintf(stderr, "unable to read DFU status\n");
		goto out_close;
	}
	printf("state(%u) = %s, status(%u) = %s\n", dst.bState,
		dfu_state_to_string(dst.bState), dst.bStatus,
		dfu_status_to_string(dst.bStatus));

	usleep(dst.bwPollTimeout * 1000);
	/* FIXME: deal correctly with ManifestationTolerant=0 / WillDetach bits */
	switch (dst.bState) {
	case DFU_STATE_dfuMANIFEST_SYNC:
	case DFU_STATE_dfuMANIFEST:
		/* some devices (e.g. TAS1020b) need some time before we
		 * can obtain the status */
		sleep(1);
		goto get_status;
		break;
	case DFU_STATE_dfuIDLE:
		break;
	}

	ret = bytes_sent;
	printf("Done!\n");
out_close:
	close(fd);
out_free:
	free(buf);

	return ret;
}

/* This is a quick and dirty rip from dfuse_do_dfuse_dnload */
/* Download raw binary file to DfuSe device */
int dfuse_do_bin_dnload(struct usb_dev_handle *usb_handle, int interface,
		      int xfer_size, const char *fname, int start_address)
{
    int dwElementAddress;
    int dwElementSize;
    char *data;
    int fd;
    struct stat st;
    int read_bytes = 0;
    int ret;
#ifdef STM32_CL
    int page_size = 2048; /* FIXME: get this from device */
#else
    int page_size = 1024; /* FIXME: get this from device */
#endif
    int p;

    fd = open(fname, O_RDONLY|O_BINARY);
    if (fd < 0) {
	perror(fname);
	return fd;
    }

    ret = fstat(fd, &st);
    if (ret < 0) {
	perror(fname);
	goto out_close;
    }

    dwElementAddress = start_address;
    dwElementSize = st.st_size;
    printf("address = 0x%08x, ", dwElementAddress);
    printf("size = %i\n", dwElementSize);

    data = malloc(dwElementSize);
    if (!data) {
	fprintf(stderr, "Could not allocate data buffer\n");
	ret = -ENOMEM;
	goto out_close;
    }
    ret = read(fd, data, dwElementSize);
    read_bytes += ret;
    if (ret < dwElementSize) {
	fprintf(stderr, "Could not read data\n");
	free(data);
	ret = -EINVAL;
	goto out_close;
    }

    /* FIXME: deal with page_size != xfer_size */
    if (xfer_size != page_size) {
	fprintf(stderr, "Transfer size different from flash page size"
			"is not supported\n");
	exit(1);
    }

    for (p = 0; p < dwElementSize; p += xfer_size) {
	int address;
	int chunk_size = xfer_size;

	address = dwElementAddress + p;
	/* DEBUG: paranoid check for DSO Nano, do not overwrite
	   the original bootloader */
#ifndef STM32_CL
	if (address < 0x08004000 ||
	    address + chunk_size > 0x08020000) {
		fprintf(stderr, "Address 0x%08x out of bounds\n", address);
		exit(1);
	}
#endif
	/* move this check inside dfuse_set_address_pointer? */
	if ((address & ~(page_size - 1)) !=
	    (last_erased & ~(page_size -1)))
		dfuse_set_address_pointer(usb_handle, interface,
					address, ERASE);
	/* check if this is the last chunk */
	if (p + chunk_size > dwElementSize)
	    chunk_size = dwElementSize - p;
	/* if not aligned on page, erase next page as well */
	if (((address + chunk_size - 1) & ~(page_size - 1)) !=
	    (last_erased & ~(page_size -1)))
		dfuse_set_address_pointer(usb_handle, interface,
					address + chunk_size, ERASE);
#ifdef DEBUG_DRY
	printf("  DEBUG_DRY: download from image offset "
			"%08x to memory %08x, size %i\n",
			p, address, chunk_size);
#else
	dfuse_set_address_pointer(usb_handle, interface,
				address, 0);
	/* transaction = 2 for no address offset */
	ret = dfuse_dnload_chunk(usb_handle, interface, data + p,
				chunk_size, 2);
	printf(" Wrote %i bytes at 0x%08x\n", ret, address);
	if (ret != chunk_size) {
		fprintf(stderr, "Failed to write whole chunk of %i bytes\n",
			chunk_size);
		free(data);
		ret = -EINVAL;
		goto out_close;
	}
#endif
    }
    free(data);

    if (read_bytes != st.st_size) {
	fprintf(stderr, "Warning: Read %i bytes, file size %i\n", read_bytes,
		(int) st.st_size);
    }
    ret = read_bytes;

out_close:
    close(fd);

    return ret;
}

/* Parse a DfuSe file and download contents to device */
int dfuse_do_dfuse_dnload(struct usb_dev_handle *usb_handle, int interface,
		      int xfer_size, const char *fname)
{
    char dfuprefix[11];
    char dfusuffix[16];
    char targetprefix[274];
    char elementheader[8];
    int image;
    int element;
    int bTargets;
    int bAlternateSetting;
    int dwNbElements;
    int dwElementAddress;
    int dwElementSize;
    char *data;
    int fd;
    struct stat st;
    int read_bytes = 0;
    int ret;
#ifdef STM32_CL
    int page_size = 2048; /* FIXME: get this from device */
#else
    int page_size = 1024; /* FIXME: get this from device */
#endif
    int p;

    fd = open(fname, O_RDONLY|O_BINARY);
    if (fd < 0) {
	perror(fname);
	return fd;
    }

    ret = fstat(fd, &st);
    if (ret < 0) {
	perror(fname);
	goto out_close;
    }

    /* Must be larger than a minimal DfuSe header and suffix */
    if (st.st_size <= 11 + 16 + 274 + 8) {
	fprintf(stderr, "File too small for a DfuSe file\n");
	ret = -EINVAL;
	goto out_close;
    }

    ret = read(fd, dfuprefix, sizeof(dfuprefix));
    read_bytes = ret;
    if (ret < sizeof(dfuprefix)) {
	fprintf(stderr, "Could not read DfuSe header\n");
	goto out_close;
    }
    if (strncmp(dfuprefix, "DfuSe", 5)) {
	fprintf(stderr, "No valid DfuSe signature\n");
	goto out_close;
    }
    if (dfuprefix[5] != 0x01) {
	fprintf(stderr, "DFU format revision %i not supported\n", dfuprefix[5]);
	goto out_close;
    }
    bTargets = dfuprefix[10];
    printf("file contains %i DFU images\n", bTargets);

    for (image = 1; image <= bTargets; image++) {
	printf("parsing DFU image %i\n", image);
	ret = read(fd, targetprefix, sizeof(targetprefix));
        read_bytes += ret;
	if (ret < sizeof(targetprefix)) {
	    fprintf(stderr, "Could not read DFU header\n");
	    goto out_close;
	}
	if (strncmp(targetprefix, "Target", 6)) {
	    fprintf(stderr, "No valid target signature\n");
	    goto out_close;
	}
	bAlternateSetting = targetprefix[6];
	dwNbElements = quad(targetprefix + 270);
	printf("image for alternate setting %i, ", bAlternateSetting);
	printf("(%i elements, ", dwNbElements);
	printf("total size = %i)\n", quad(targetprefix + 266));
	for (element = 1; element <= dwNbElements; element++) {
	    printf("parsing element %i, ", element);
	    ret = read(fd, elementheader, sizeof(elementheader));
	    read_bytes += ret;
	    if (ret < sizeof(elementheader)) {
		fprintf(stderr, "Could not read element header\n");
		goto out_close;
	    }
	    dwElementAddress = quad(elementheader);
	    dwElementSize = quad(elementheader + 4);
	    printf("address = 0x%08x, ", dwElementAddress);
	    printf("size = %i\n", dwElementSize);
	    /* sanity check */
	    if (read_bytes + dwElementSize + sizeof(dfusuffix) > st.st_size) {
		fprintf(stderr, "File too small for element size\n");
		goto out_close;
	    }
	    data = malloc(dwElementSize);
	    if (!data) {
		fprintf(stderr, "Could not allocate data buffer\n");
		ret = -ENOMEM;
		goto out_close;
	    }
	    ret = read(fd, data, dwElementSize);
	    read_bytes += ret;
	    if (ret < dwElementSize) {
		fprintf(stderr, "Could not read data\n");
	        free(data);
		ret = -EINVAL;
		goto out_close;
	    }

	    /* FIXME: deal with page_size != xfer_size */
	    if (xfer_size != page_size) {
		fprintf(stderr, "Transfer size different from flash page size"
				"is not supported\n");
		exit(1);
	    }

	    for (p = 0; p < dwElementSize; p += xfer_size) {
		int address;
		int chunk_size = xfer_size;

		address = dwElementAddress + p;
		/* DEBUG: paranoid check for DSO Nano, do not overwrite
		   the original bootloader */
#ifndef STM32_CL
		if (address < 0x08004000 ||
	  	    address + chunk_size > 0x08020000) {
			fprintf(stderr, "Address 0x%08x out of bounds\n", address);
			exit(1);
		}
#endif
		/* move this check inside dfuse_set_address_pointer? */
		if ((address & ~(page_size - 1)) !=
                    (last_erased & ~(page_size -1)))
			dfuse_set_address_pointer(usb_handle, interface,
						address, ERASE);
		/* check if this is the last chunk */
		if (p + chunk_size > dwElementSize)
		    chunk_size = dwElementSize - p;
		/* if not aligned on page, erase next page as well */
		if (((address + chunk_size - 1) & ~(page_size - 1)) !=
		    (last_erased & ~(page_size -1)))
			dfuse_set_address_pointer(usb_handle, interface,
						address + chunk_size, ERASE);
#ifdef DEBUG_DRY
		printf("  DEBUG_DRY: download from image offset "
				"%08x to memory %08x, size %i\n",
				p, address, chunk_size);
#else
		dfuse_set_address_pointer(usb_handle, interface,
					address, 0);
		/* transaction = 2 for no address offset */
		ret = dfuse_dnload_chunk(usb_handle, interface, data + p,
					chunk_size, 2);
		printf(" Wrote %i bytes at 0x%08x\n", ret, address);
		if (ret != chunk_size) {
			fprintf(stderr, "Failed to write whole chunk "
					"of %i bytes\n", chunk_size);
			free(data);
			ret = -EINVAL;
			goto out_close;
		}
#endif
	    }
	    free(data);
	}
    }

    /* FIXME: seek and read this before anything else */
    /* and share it with the standard DFU code */
    ret = read(fd, dfusuffix, sizeof(dfusuffix));
    read_bytes += ret;
    if (ret < sizeof(dfusuffix)) {
	fprintf(stderr, "Could not read DFU suffix\n");
	ret = -EINVAL;
	goto out_close;
    }
    if (strncmp(dfusuffix + 8, "UFD", 3)) {
	fprintf(stderr, "No valid DFU signature\n");
	ret = -EINVAL;
	goto out_close;
    }
    if (dfusuffix[6] != 0x1a || dfusuffix[7] != 0x01) {
	fprintf(stderr, "Not supported DfuSe version\n");
	ret = -EINVAL;
	goto out_close;
    }

    if (read_bytes != st.st_size) {
	fprintf(stderr, "Warning: Read %i bytes, file size %i\n", read_bytes,
		(int) st.st_size);
    }

    printf("done parsing DfuSe file\n");
    ret = read_bytes;

out_close:
    close(fd);

    return ret;
}
