#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#include "../command.h"

#define FX3_VID 0x04b4
#define FX3_PID 0x00f3
#define FX3_RAM_REQUEST 0xa0
#define FX3_MAX_WRITE_SIZE (2 * 1024)
#define USB_TIMEOUT_MS 5000
#define RENUM_RETRIES 200
#define MAX_PORT_DEPTH 8

struct usb_path {
	uint8_t bus;
	uint8_t ports[MAX_PORT_DEPTH];
	int depth;
};

struct upload_context {
	libusb_device_handle *handle;
};

static uint32_t read_le32(const unsigned char *data)
{
	return (uint32_t)data[0] |
		((uint32_t)data[1] << 8) |
		((uint32_t)data[2] << 16) |
		((uint32_t)data[3] << 24);
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
	if (parsed_bus > UINT8_MAX || parsed_addr > UINT8_MAX)
		return -1;

	*bus = (uint8_t)parsed_bus;
	*addr = (uint8_t)parsed_addr;
	return 0;
}

static int parse_usb_id(const char *text, uint16_t *vid, uint16_t *pid)
{
	const char *hex = "0123456789abcdefABCDEF";

	if (strlen(text) != 9 || text[4] != ':' ||
			strspn(text, hex) != 4 || strspn(text + 5, hex) != 4)
		return -1;
	*vid = (uint16_t)strtoul(text, NULL, 16);
	*pid = (uint16_t)strtoul(text + 5, NULL, 16);
	return 0;
}

static int get_usb_path(libusb_device *dev, struct usb_path *path)
{
	int ret;

	path->bus = libusb_get_bus_number(dev);
	ret = libusb_get_port_numbers(dev, path->ports, sizeof(path->ports));
	if (ret < 0)
		return ret;
	path->depth = ret;
	return 0;
}

static int usb_path_equal(libusb_device *dev, const struct usb_path *path)
{
	struct usb_path candidate;

	if (get_usb_path(dev, &candidate) < 0)
		return 0;
	if (candidate.bus != path->bus || candidate.depth != path->depth)
		return 0;
	return !memcmp(candidate.ports, path->ports, path->depth);
}

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
	if (ret < 0) {
		buf[0] = '\0';
		return ret;
	}
	buf[len - 1] = '\0';
	return 0;
}

static int get_identity(libusb_device *dev, libusb_device_handle *handle,
		char *manufacturer, size_t manufacturer_len,
		char *product, size_t product_len)
{
	struct libusb_device_descriptor desc;

	manufacturer[0] = product[0] = '\0';
	if (libusb_get_device_descriptor(dev, &desc) < 0)
		return -1;
	if (get_string(handle, desc.iManufacturer,
			manufacturer, manufacturer_len) < 0)
		manufacturer[0] = '\0';
	if (get_string(handle, desc.iProduct, product, product_len) < 0)
		product[0] = '\0';
	return 0;
}

static int is_sigrok_firmware(const char *manufacturer, const char *product)
{
	return !strcmp(manufacturer, "sigrok") &&
		(!strcmp(product, "fx3lafw") || !strcmp(product, "fx3ulpifw"));
}

static int is_fx3_bootloader(const char *manufacturer, const char *product)
{
	return !strcmp(product, "WestBridge") &&
		(!strcmp(manufacturer, "Cypress") ||
		 !strcmp(manufacturer, "Cypress Semiconductor"));
}

static int is_fx3_bootloader_device(libusb_device *dev)
{
	struct libusb_device_descriptor desc;

	if (libusb_get_device_descriptor(dev, &desc) < 0)
		return 0;
	return desc.idVendor == FX3_VID && desc.idProduct == FX3_PID &&
		desc.bcdDevice == 0x0100 && desc.bDeviceClass == 0;
}

static libusb_device *find_device(libusb_context *ctx,
		uint16_t vid, uint16_t pid,
		int have_conn, uint8_t want_bus, uint8_t want_addr,
		const struct usb_path *want_path)
{
	libusb_device **devices;
	libusb_device *selected;
	struct libusb_device_descriptor desc;
	ssize_t count, i;
	unsigned int matches;

	count = libusb_get_device_list(ctx, &devices);
	if (count < 0)
		return NULL;

	selected = NULL;
	matches = 0;
	for (i = 0; i < count; i++) {
		if (libusb_get_device_descriptor(devices[i], &desc) < 0)
			continue;
		if (desc.idVendor != vid || desc.idProduct != pid)
			continue;
		if (have_conn &&
				(libusb_get_bus_number(devices[i]) != want_bus ||
				libusb_get_device_address(devices[i]) != want_addr))
			continue;
		if (want_path && !usb_path_equal(devices[i], want_path))
			continue;
		selected = devices[i];
		matches++;
	}

	if (matches == 1)
		libusb_ref_device(selected);
	else
		selected = NULL;
	libusb_free_device_list(devices, 1);

	if (matches > 1)
		fprintf(stderr,
			"More than one FX3 device found; specify bus.address.\n");
	return selected;
}

static int reset_loaded_firmware(libusb_device_handle *handle)
{
	int ret;

	ret = libusb_control_transfer(handle,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
			LIBUSB_ENDPOINT_OUT,
		CMD_RESET, 0, 0, NULL, 0, USB_TIMEOUT_MS);
	if (ret == 0 || ret == LIBUSB_ERROR_NO_DEVICE ||
			ret == LIBUSB_ERROR_IO)
		return 0;

	fprintf(stderr, "Unable to reset loaded firmware: %s\n",
		libusb_error_name(ret));
	return -1;
}

static int parse_image(const unsigned char *image, size_t length,
		int (*write_cb)(uint32_t, const unsigned char *, size_t, void *),
		void *cb_data, uint32_t *entry_addr)
{
	size_t offset, i, segment_bytes;
	uint32_t checksum, expected_checksum;

	if (length < 16 || image[0] != 'C' || image[1] != 'Y' ||
			(image[2] & 1) || image[3] != 0xb0)
		return -1;

	checksum = 0;
	offset = 4;
	while (offset < length) {
		uint32_t words, address;

		if (length - offset < 8)
			return -1;
		words = read_le32(image + offset);
		address = read_le32(image + offset + 4);
		offset += 8;

		if (words == 0) {
			if (length - offset < 4)
				return -1;
			expected_checksum = read_le32(image + offset);
			if (checksum != expected_checksum)
				return -1;
			*entry_addr = address;
			return 0;
		}

#if SIZE_MAX < UINT64_MAX
		if ((uint64_t)words * 4 > SIZE_MAX)
			return -1;
#endif
		segment_bytes = (size_t)words * 4;
		if (segment_bytes > length - offset)
			return -1;

		for (i = 0; i < segment_bytes; i += 4)
			checksum += read_le32(image + offset + i);

		if (write_cb && write_cb(address, image + offset,
				segment_bytes, cb_data) < 0)
			return -1;
		offset += segment_bytes;
	}

	return -1;
}

static int write_ram(uint32_t address, const unsigned char *data,
		size_t length, void *cb_data)
{
	struct upload_context *upload;
	size_t offset, chunk;
	int ret;

	upload = cb_data;
	for (offset = 0; offset < length; offset += chunk) {
		chunk = length - offset;
		if (chunk > FX3_MAX_WRITE_SIZE)
			chunk = FX3_MAX_WRITE_SIZE;
		ret = libusb_control_transfer(upload->handle,
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
				LIBUSB_ENDPOINT_OUT,
			FX3_RAM_REQUEST, (address + offset) & 0xffff,
			(address + offset) >> 16,
			(unsigned char *)data + offset, (uint16_t)chunk,
			USB_TIMEOUT_MS);
		if (ret != (int)chunk) {
			fprintf(stderr, "RAM write at 0x%08x failed: %s\n",
				(unsigned int)(address + offset),
				ret < 0 ? libusb_error_name(ret) : "short write");
			return -1;
		}
	}

	return 0;
}

/*
 * Read every uploaded segment back out of SRAM through the same bootloader
 * vendor request. This separates "the image never made it into SRAM" from
 * "the image is there but does not execute" when the firmware fails to
 * re-enumerate.
 */
static int verify_ram(uint32_t address, const unsigned char *data,
		size_t length, void *cb_data)
{
	struct upload_context *upload;
	unsigned char buf[FX3_MAX_WRITE_SIZE];
	size_t offset, chunk, i;
	int ret;

	upload = cb_data;
	for (offset = 0; offset < length; offset += chunk) {
		chunk = length - offset;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);
		ret = libusb_control_transfer(upload->handle,
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
				LIBUSB_ENDPOINT_IN,
			FX3_RAM_REQUEST, (address + offset) & 0xffff,
			(address + offset) >> 16, buf, (uint16_t)chunk,
			USB_TIMEOUT_MS);
		if (ret != (int)chunk) {
			fprintf(stderr, "RAM read at 0x%08x failed: %s\n",
				(unsigned int)(address + offset),
				ret < 0 ? libusb_error_name(ret) : "short read");
			return -1;
		}
		for (i = 0; i < chunk; i++) {
			if (buf[i] == data[offset + i])
				continue;
			fprintf(stderr,
				"RAM mismatch at 0x%08x: wrote %02x, read %02x\n",
				(unsigned int)(address + offset + i),
				data[offset + i], buf[i]);
			return -1;
		}
	}

	return 0;
}

static unsigned char *read_file(const char *path, size_t *length)
{
	unsigned char *data;
	FILE *file;
	long size;

	file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "Unable to open %s: %s\n", path, strerror(errno));
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) < 0 ||
			(size = ftell(file)) < 0 ||
			fseek(file, 0, SEEK_SET) < 0) {
		fprintf(stderr, "Unable to determine firmware size.\n");
		fclose(file);
		return NULL;
	}
	if (size == 0
#if LONG_MAX > SIZE_MAX
			|| (unsigned long)size > SIZE_MAX
#endif
			) {
		fprintf(stderr, "Invalid firmware size: %ld\n", size);
		fclose(file);
		return NULL;
	}

	data = malloc((size_t)size);
	if (!data) {
		fclose(file);
		return NULL;
	}
	if (fread(data, 1, (size_t)size, file) != (size_t)size) {
		fprintf(stderr, "Unable to read complete firmware image.\n");
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*length = (size_t)size;
	return data;
}

static const char *expected_product(const char *path)
{
	const char *basename = strrchr(path, '/');

	if (basename)
		path = basename + 1;
	if (strstr(path, "fx3ulpifw"))
		return "fx3ulpifw";
	if (strstr(path, "fx3lafw"))
		return "fx3lafw";
	return NULL;
}

static libusb_device *wait_for_device(libusb_context *ctx,
		const struct usb_path *path, uint16_t vid, uint16_t pid,
		const char *product)
{
	libusb_device *dev;
	libusb_device_handle *handle;
	char manufacturer[64], found_product[64];
	int retry;

	for (retry = 0; retry < RENUM_RETRIES; retry++) {
		dev = find_device(ctx, vid, pid, 0, 0, 0, path);
		/*
		 * macOS exposes the USB 2 bootloader and USB 3 firmware through
		 * different companion-port paths. Fall back to the only FX3 device;
		 * find_device() rejects ambiguous multi-device setups.
		 */
		if (!dev)
			dev = find_device(ctx, vid, pid, 0, 0, 0, NULL);
		if (!dev) {
			usleep(100000);
			continue;
		}
		if (product && !*product && is_fx3_bootloader_device(dev))
			return dev;
		/* The old BootROM may remain visible after the launch request. */
		if ((!product || *product) && is_fx3_bootloader_device(dev)) {
			libusb_unref_device(dev);
			usleep(100000);
			continue;
		}
		handle = NULL;
		if (libusb_open(dev, &handle) == 0) {
			int ret = get_identity(dev, handle, manufacturer,
				sizeof(manufacturer), found_product, sizeof(found_product));
			libusb_close(handle);
			if (ret == 0 && ((!product &&
					!is_fx3_bootloader(manufacturer, found_product)) ||
					(product && !*product &&
					 is_fx3_bootloader(manufacturer, found_product)) ||
					(product && *product && !strcmp(manufacturer, "sigrok") &&
					 !strcmp(found_product, product))))
				return dev;
		}
		libusb_unref_device(dev);
		usleep(100000);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	libusb_context *ctx;
	libusb_device *dev;
	libusb_device_handle *handle;
	struct usb_path path;
	struct upload_context upload;
	unsigned char *image;
	const char *expect_product;
	char manufacturer[64], product[64];
	size_t image_length;
	uint32_t entry_addr;
	uint16_t expect_vid = FX3_VID, expect_pid = FX3_PID;
	uint8_t want_bus, want_addr;
	int have_conn, ret, verify, no_run, argi, have_expect_usb = 0;

	verify = no_run = 0;
	for (argi = 1; argi < argc && argv[argi][0] == '-'; argi++) {
		if (!strcmp(argv[argi], "--verify"))
			verify = 1;
		else if (!strcmp(argv[argi], "--no-run"))
			no_run = 1;
		else if (!strcmp(argv[argi], "--expect-usb")) {
			if (++argi >= argc ||
					parse_usb_id(argv[argi], &expect_vid, &expect_pid) < 0) {
				fprintf(stderr, "--expect-usb requires VID:PID (four hex digits each).\n");
				return 2;
			}
			have_expect_usb = 1;
		}
		else {
			fprintf(stderr, "Unknown option: %s\n", argv[argi]);
			return 2;
		}
	}

	if (argc - argi < 1 || argc - argi > 2) {
		fprintf(stderr,
			"Usage: %s [--verify] [--no-run] [--expect-usb VID:PID] firmware.fw "
			"[bus.address]\n", argv[0]);
		return 2;
	}

	expect_product = expected_product(argv[argi]);
	if (!expect_product && !have_expect_usb && !no_run) {
		fprintf(stderr, "Unknown firmware identity; specify --expect-usb VID:PID.\n");
		return 2;
	}

	have_conn = argc - argi == 2;
	want_bus = want_addr = 0;
	if (have_conn && parse_conn(argv[argi + 1], &want_bus, &want_addr) < 0) {
		fprintf(stderr, "Invalid bus.address: %s\n", argv[argi + 1]);
		return 2;
	}

	image = read_file(argv[argi], &image_length);
	if (!image)
		return 1;
	if (parse_image(image, image_length, NULL, NULL, &entry_addr) < 0) {
		fprintf(stderr, "Invalid Cypress FX3 firmware image.\n");
		free(image);
		return 1;
	}

	ctx = NULL;
	if (libusb_init(&ctx) < 0) {
		free(image);
		return 1;
	}

	dev = find_device(ctx, FX3_VID, FX3_PID, have_conn, want_bus, want_addr, NULL);
	if (!dev) {
		fprintf(stderr, "No matching Cypress FX3 device found.\n");
		ret = 1;
		goto out;
	}
	if (get_usb_path(dev, &path) < 0) {
		fprintf(stderr, "Unable to read the FX3 physical USB path.\n");
		ret = 1;
		goto out_dev;
	}

	handle = NULL;
	if (libusb_open(dev, &handle) < 0) {
		fprintf(stderr, "Unable to open the FX3 device.\n");
		ret = 1;
		goto out_dev;
	}
	get_identity(dev, handle, manufacturer, sizeof(manufacturer),
		product, sizeof(product));

	if (is_sigrok_firmware(manufacturer, product)) {
		printf("Resetting %s on %u.%u\n", product,
			libusb_get_bus_number(dev), libusb_get_device_address(dev));
		if (reset_loaded_firmware(handle) < 0) {
			ret = 1;
			goto out_handle;
		}
		libusb_close(handle);
		handle = NULL;
		libusb_unref_device(dev);
		dev = wait_for_device(ctx, &path, FX3_VID, FX3_PID, "");
		if (!dev) {
			fprintf(stderr, "FX3 did not return in USB boot mode.\n");
			ret = 1;
			goto out;
		}
		if (libusb_open(dev, &handle) < 0) {
			fprintf(stderr, "Unable to open FX3 USB bootloader.\n");
			ret = 1;
			goto out_dev;
		}
	}

	printf("Uploading %s to %u.%u\n", argv[argi],
		libusb_get_bus_number(dev), libusb_get_device_address(dev));
	upload.handle = handle;
	if (parse_image(image, image_length, write_ram, &upload,
			&entry_addr) < 0) {
		fprintf(stderr, "Firmware upload failed.\n");
		ret = 1;
		goto out_handle;
	}

	if (verify) {
		if (parse_image(image, image_length, verify_ram, &upload,
				&entry_addr) < 0) {
			fprintf(stderr, "Firmware verification failed.\n");
			ret = 1;
			goto out_handle;
		}
		printf("Verified %zu bytes in SRAM; entry 0x%08x\n",
			image_length, (unsigned int)entry_addr);
	}

	if (no_run) {
		printf("Not launching firmware (--no-run).\n");
		ret = 0;
		goto out_handle;
	}

	ret = libusb_control_transfer(handle,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
			LIBUSB_ENDPOINT_OUT,
		FX3_RAM_REQUEST, entry_addr & 0xffff, entry_addr >> 16,
		NULL, 0, USB_TIMEOUT_MS);
	if (ret < 0 && ret != LIBUSB_ERROR_NO_DEVICE &&
			ret != LIBUSB_ERROR_IO) {
		fprintf(stderr, "Unable to launch firmware: %s\n",
			libusb_error_name(ret));
		ret = 1;
		goto out_handle;
	}

	libusb_close(handle);
	handle = NULL;
	libusb_unref_device(dev);
	dev = NULL;

	dev = wait_for_device(ctx, &path, expect_vid, expect_pid, expect_product);
	if (!dev) {
		fprintf(stderr, "FX3 firmware did not re-enumerate as %04x:%04x",
			expect_vid, expect_pid);
		if (expect_product)
			fprintf(stderr, " as sigrok/%s", expect_product);
		fprintf(stderr, ".\n");
		ret = 1;
		goto out;
	}

	printf("Firmware enumerated on %u.%u (%04x:%04x, USB speed code %d)",
		libusb_get_bus_number(dev), libusb_get_device_address(dev),
		expect_vid, expect_pid, libusb_get_device_speed(dev));
	if (expect_product)
		printf(" as sigrok/%s", expect_product);
	printf("\n");
	ret = 0;

out_dev:
	if (dev)
		libusb_unref_device(dev);
	goto out;
out_handle:
	if (handle)
		libusb_close(handle);
	if (dev)
		libusb_unref_device(dev);
out:
	if (ctx)
		libusb_exit(ctx);
	free(image);
	return ret;
}
