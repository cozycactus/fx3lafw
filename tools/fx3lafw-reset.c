#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#include "../command.h"

#define FX3LAFW_VID 0x04b4
#define FX3LAFW_PID 0x00f3
#define USB_TIMEOUT_MS 1000

static int get_string(libusb_device_handle *handle, uint8_t index,
		      char *buf, size_t len)
{
	int ret;

	if (!index) {
		buf[0] = '\0';
		return 0;
	}

	ret = libusb_get_string_descriptor_ascii(handle, index,
		(unsigned char *)buf, (int)len);
	if (ret < 0)
		return ret;
	buf[len - 1] = '\0';
	return 0;
}

static int parse_conn(const char *text, uint8_t *bus, uint8_t *addr)
{
	char *end;
	unsigned long parsed_bus, parsed_addr;

	errno = 0;
	parsed_bus = strtoul(text, &end, 10);
	if (errno || end == text || *end != '.')
		return -1;

	text = end + 1;
	errno = 0;
	parsed_addr = strtoul(text, &end, 10);
	if (errno || end == text || *end != '\0')
		return -1;
	if (parsed_bus > 255 || parsed_addr > 255)
		return -1;

	*bus = (uint8_t)parsed_bus;
	*addr = (uint8_t)parsed_addr;
	return 0;
}

static int reset_device(libusb_device_handle *handle)
{
	int ret;

	ret = libusb_control_transfer(handle,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
			LIBUSB_ENDPOINT_OUT,
		CMD_RESET, 0, 0, NULL, 0, USB_TIMEOUT_MS);
	if (ret == 0)
		return 0;

	if (ret == LIBUSB_ERROR_NO_DEVICE || ret == LIBUSB_ERROR_IO) {
		fprintf(stderr,
			"Device disappeared during reset command; treating as reset.\n");
		return 0;
	}

	fprintf(stderr, "Unable to send reset command: %s\n",
		libusb_error_name(ret));
	return 1;
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device **devices = NULL;
	libusb_device_handle *handle = NULL;
	struct libusb_device_descriptor desc;
	ssize_t count;
	uint8_t want_bus = 0, want_addr = 0;
	int have_conn = 0;
	int ret = 1;

	if (argc > 2) {
		fprintf(stderr, "Usage: %s [bus.address]\n", argv[0]);
		return 2;
	}
	if (argc == 2) {
		if (parse_conn(argv[1], &want_bus, &want_addr) < 0) {
			fprintf(stderr, "Invalid bus.address: %s\n", argv[1]);
			return 2;
		}
		have_conn = 1;
	}

	if (libusb_init(&ctx) < 0) {
		fprintf(stderr, "Unable to initialize libusb.\n");
		return 1;
	}

	count = libusb_get_device_list(ctx, &devices);
	if (count < 0) {
		fprintf(stderr, "Unable to list USB devices: %s\n",
			libusb_error_name((int)count));
		goto out;
	}

	for (ssize_t i = 0; i < count; i++) {
		libusb_device *dev = devices[i];
		uint8_t bus = libusb_get_bus_number(dev);
		uint8_t addr = libusb_get_device_address(dev);
		char manufacturer[128];
		char product[128];

		if (have_conn && (bus != want_bus || addr != want_addr))
			continue;

		if (libusb_get_device_descriptor(dev, &desc) < 0)
			continue;
		if (desc.idVendor != FX3LAFW_VID || desc.idProduct != FX3LAFW_PID)
			continue;

		if (libusb_open(dev, &handle) < 0)
			continue;

		if (get_string(handle, desc.iManufacturer, manufacturer,
				sizeof(manufacturer)) < 0 ||
				get_string(handle, desc.iProduct, product,
				sizeof(product)) < 0) {
			libusb_close(handle);
			handle = NULL;
			continue;
		}

		if (strcmp(manufacturer, "sigrok") ||
				strcmp(product, "fx3lafw")) {
			libusb_close(handle);
			handle = NULL;
			continue;
		}

		printf("Resetting fx3lafw on %u.%u\n", bus, addr);
		ret = reset_device(handle);
		libusb_close(handle);
		handle = NULL;
		goto out;
	}

	fprintf(stderr, "No loaded sigrok/fx3lafw device found");
	if (have_conn)
		fprintf(stderr, " at %u.%u", want_bus, want_addr);
	fprintf(stderr, ".\n");

out:
	if (devices)
		libusb_free_device_list(devices, 1);
	libusb_exit(ctx);
	return ret;
}
