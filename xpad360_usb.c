#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb-1.0/libusb.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <endian.h>
#include <linux/uinput.h>

#include "xpad360_threadpool.h"
#include "xpad360_uinput.h"

/* Note that the below doesn't matter since we use a custom version of libusb anyways.  */
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION < 0x01000102)
	#error This driver requires hotplug support. libusb headers indicate the version you have is not new enough. 
#endif

static const int xpad360_evbit[] = {
	EV_KEY, EV_ABS, EV_FF
};

static const int xpad360_keybit[] = {
	BTN_A, BTN_B, BTN_X, BTN_Y,
	BTN_START, BTN_SELECT,
	BTN_THUMBL, BTN_THUMBR,
	BTN_TL, BTN_TR, BTN_MODE
};

static const int xpad360_absbit[] = {
	ABS_X, ABS_Y, ABS_Z,
	ABS_RX, ABS_RY, ABS_RZ,
	ABS_HAT0X, ABS_HAT0Y
};

static const int xpad360_ffbit[] = { FF_RUMBLE };

static const int *xpad360_feature_tables[] = {
	xpad360_evbit,
	xpad360_keybit,
	xpad360_absbit,
	xpad360_ffbit
};

static const int xpad360_table_sizes[] = {
	(sizeof(xpad360_evbit) / sizeof(xpad360_evbit[0])),
	(sizeof(xpad360_keybit) / sizeof(xpad360_keybit[0])),
	(sizeof(xpad360_absbit) / sizeof(xpad360_absbit[0])),
	(sizeof(xpad360_ffbit) / sizeof(xpad360_ffbit[0]))
};

static const int xpad360_feature_constants[] = {
	UI_SET_EVBIT,
	UI_SET_KEYBIT,
	UI_SET_ABSBIT,
	UI_SET_FFBIT
};

const int xpad360_num_features = sizeof(xpad360_feature_constants) / sizeof(xpad360_feature_constants[0]);

static int xpad360_connect(libusb_context *ctx, libusb_device *device);
static void xpad360_disconnect(libusb_context *ctx, libusb_device *device);

static const libusb_hotplug driver = {
	.connect = xpad360_connect,
	.disconnect = xpad360_disconnect,
	.vid = 0x045e, .pid = 0x028e,
	.dev_class = LIBUSB_HOTPLUG_MATCH_ANY,
	.flags = LIBUSB_HOTPLUG_ENUMERATE
};

struct xpad360_endpoint {
	struct libusb_transfer *urb;
	unsigned char data[32];
};

struct xpad360_controller {
	libusb_device_handle *dev_handle;
	struct xpad360_endpoint in;
	int uinput_fd;
};

inline static int xpad360_proc_status(enum libusb_transfer_status status)
{
	switch(status) {
	case LIBUSB_TRANSFER_COMPLETED: 
		return 0;
	case LIBUSB_TRANSFER_ERROR:
	case LIBUSB_TRANSFER_TIMED_OUT:
	case LIBUSB_TRANSFER_CANCELLED:
	case LIBUSB_TRANSFER_STALL:
	case LIBUSB_TRANSFER_NO_DEVICE:
	case LIBUSB_TRANSFER_OVERFLOW:
		printf("Transfer error: %s\n", libusb_error_name(status));
		return 1;
	default:
		printf("Unknown error occured.\n");
		return -1;
	}
}

inline static void xpad360_proc_input(int fd, unsigned char *data)
{
	uinput_report_abs(fd, ABS_HAT0X, !!(data[0] & 0x08) - !!(data[0] & 0x04));
	uinput_report_abs(fd, ABS_HAT0Y, !!(data[0] & 0x02) - !!(data[0] & 0x01));
	
	/* start/back buttons */
	uinput_report_key(fd, BTN_START,  data[0] & 0x10);
	uinput_report_key(fd, BTN_SELECT, data[0] & 0x20); /* Back */

	/* stick press left/right */
	uinput_report_key(fd, BTN_THUMBL, data[0] & 0x40);
	uinput_report_key(fd, BTN_THUMBR, data[0] & 0x80);

	uinput_report_key(fd, BTN_TL, data[1] & 0x01); /* Left Shoulder */
	uinput_report_key(fd, BTN_TR, data[1] & 0x02); /* Right Shoulder */
	uinput_report_key(fd, BTN_MODE, data[1] & 0x04); /* Guide */
	/* data[8] & 0x08 is a dummy value */
	uinput_report_key(fd, BTN_A, data[1] & 0x10);
	uinput_report_key(fd, BTN_B, data[1] & 0x20);
	uinput_report_key(fd, BTN_X, data[1] & 0x40);
	uinput_report_key(fd, BTN_Y, data[1] & 0x80);

	uinput_report_abs(fd, ABS_Z, data[2]);
	uinput_report_abs(fd, ABS_RZ, data[3]);

	/* Left Stick */
	uinput_report_abs(fd, ABS_X, (__s16)le16_to_cpup((__le16*)&data[4]));
	uinput_report_abs(fd, ABS_Y, ~(__s16)le16_to_cpup((__le16*)&data[6]));

	/* Right Stick */
	uinput_report_abs(fd, ABS_RX, (__s16)le16_to_cpup((__le16*)&data[8]));
	uinput_report_abs(fd, ABS_RY, ~(__s16)le16_to_cpup((__le16*)&data[10]));
	
	uinput_sync(fd);
}

static void process_data(unsigned char *data, void *ctx)
{
	struct xpad360_controller *controller = ctx;
	__u16 header = le16_to_cpup((__le16*)&data[0]);
	
	switch(header) {
	case 0x0301:
		printf("Controller LED status: %i\n", data[2]);
		break;
	case 0x0303:
		printf("Rumble packet or something... I dunno. Have some info: %i\n", data[2]);
		break;
	case 0x0308:
		printf("Attachment attached! We don't support any of them. );");
		break;
	case 0x1400:
		xpad360_proc_input(controller->uinput_fd, &data[2]);
	}
}

static void xpad360_receive(struct libusb_transfer *transfer)
{
	int error = 0;
	
	error = xpad360_proc_status(transfer->status);
	if (error) {
		printf("Skipping transfer submission...\n");
		goto error;
	}
	
	pool_queue_work(transfer->buffer, transfer->user_data);
	
	error = libusb_submit_transfer(transfer);
	
	if (error) {
		printf("Failed to submit URB. Try to replug device.\n");
		goto error;
	}

	return; 
	
error:
	libusb_free_transfer(transfer);
}

static int xpad360_connect(libusb_context *ctx, libusb_device *device)
{	
	struct xpad360_controller *controller = calloc(1, sizeof(struct xpad360_controller));
	int error = 0;

	if (!controller) {
		printf("Failed to allocate resources for controller!\n");
		return 1;
	}
	
	error = libusb_open(device, &controller->dev_handle);
	if (error) {
		printf("Failed to open device: %s\n", libusb_error_name(error));
		goto fail_open;
	}

	controller->in.urb = libusb_alloc_transfer(0);
	if (!controller->in.urb) {
		printf("Failed to allocate transfer URB.\n");
		goto fail_alloc;
	}

	error = libusb_claim_interface(controller->dev_handle, 0);
	if (error) {
		printf("Failed to claim interface: %s\n", libusb_error_name(error));
		goto fail_claim;
	}

	libusb_fill_interrupt_transfer(
		controller->in.urb, controller->dev_handle,
		0x81, /* Endpoint 1, Direction IN */
		controller->in.data, 32, 
		xpad360_receive, controller, 0);

	controller->uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (controller->uinput_fd < 0) {
		printf("Failed to open uinput device: %s\n", strerror(errno));
		goto fail_uinput;
	}
		
	for (int i = 0; i < xpad360_num_features; ++i) {
		for (int j = 0; j < xpad360_table_sizes[i]; ++j) {
			const int constant = xpad360_feature_constants[i];
			const int feature = xpad360_feature_tables[i][j];
			
			error = ioctl(controller->uinput_fd, constant, feature);
			
			if (error < 0) {
				printf("Feature ioctl failed: %s\n", strerror(errno));
				error = 0; /* Not fatal...? */
			}
		}
	}
	
	{
		struct uinput_user_dev uinput_dev = { { 0 } };
		
		snprintf(uinput_dev.name, UINPUT_MAX_NAME_SIZE, "Xbox 360 Wired Controller");
		uinput_dev.id.bustype = BUS_USB;
		uinput_dev.id.vendor = driver.vid;
		uinput_dev.id.product = driver.pid;
		uinput_dev.id.version = 1; /* Doesn't matter */
		
		uinput_set_abs_params(&uinput_dev, ABS_X, -32768, 32768, 16, 128);
		uinput_set_abs_params(&uinput_dev, ABS_RX, -32768, 32768, 16, 128);
		uinput_set_abs_params(&uinput_dev, ABS_Y, -32768, 32768, 16, 128);
		uinput_set_abs_params(&uinput_dev, ABS_RY, -32768, 32768, 16, 128);
		uinput_set_abs_params(&uinput_dev, ABS_HAT0X, -1, 1, 0, 0);
		uinput_set_abs_params(&uinput_dev, ABS_HAT0Y, -1, 1, 0, 0);
		uinput_set_abs_params(&uinput_dev, ABS_Z, 0, 255, 0, 0);
		uinput_set_abs_params(&uinput_dev, ABS_RZ, 0, 255, 0, 0);
	
		error = write(controller->uinput_fd, 
			&uinput_dev, 
			sizeof(uinput_dev));
	}
	
	if (error < 0) {
		printf("Failed to write to uinput device: %s\n", strerror(errno));
		goto fail_write;
	}
	
	error = ioctl(controller->uinput_fd, UI_DEV_CREATE);
	if (error < 0) {
		printf("Failed to create input device: %s\n", strerror(errno));
		goto fail_dev_create;
	}
	
	error = libusb_submit_transfer(controller->in.urb);
	if (error) {
		printf("Failed to submit URB. Try to replug device.\n");
		goto fail_transfer;
	}
	     
	libusb_set_device_user_data(device, controller);
	goto success;

fail_transfer:
	ioctl(controller->uinput_fd, UI_DEV_DESTROY);
fail_dev_create:
fail_write:
	close(controller->uinput_fd);
fail_uinput:
	libusb_release_interface(controller->dev_handle, 0);
fail_claim:
	libusb_free_transfer(controller->in.urb);
fail_alloc:
	libusb_close(controller->dev_handle);
fail_open:
	free(controller);
success:
	printf("Connected!\n");
	return error;
}

static void xpad360_disconnect(libusb_context *ctx, libusb_device *device)
{
	struct xpad360_controller *controller = libusb_get_device_user_data(device);

	/* Transfer callbacks are called with disconnect events.
	   No need (from what I see) to handle them explicitly */
	ioctl(controller->uinput_fd, UI_DEV_DESTROY);
	close(controller->uinput_fd);
	libusb_release_interface(controller->dev_handle, 0);
	libusb_close(controller->dev_handle);
	free(controller);
	printf("Disconnected!\n");
}

int main()
{
	int error = 0;
	libusb_context *usb_ctx;

	error = libusb_init(&usb_ctx);

	if (error) {
		printf("Failed to initialize libusb: %s\n", libusb_error_name(error));
		return 1;
	}

	if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
		printf("libusb does not support hotplug for this system.\n");
		goto fail_hotplug;
	}

	error = libusb_hotplug_register(usb_ctx, &driver);

	if (error) goto fail_hotplug;

	error = pool_init(process_data);
	if (error) {
		printf("Failed to initialize worker threads.\n");
		goto fail_pool;
	}
	
	for (;;) libusb_handle_events_completed(usb_ctx, NULL);
	
	goto success;

fail_pool:
fail_hotplug:
	printf("Failed to register hotplug driver: %s\n", libusb_error_name(error));
success:
	libusb_exit(usb_ctx); /* Deregisters for us. */
	pool_destroy();

	return error;
}
