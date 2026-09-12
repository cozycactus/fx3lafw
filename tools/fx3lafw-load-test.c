#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

struct fake_device {
	struct libusb_device_descriptor desc;
	const char *manufacturer, *product;
	uint8_t bus, address, port;
};

static struct fake_device fake[2];
static libusb_device *device_list[3];
static ssize_t device_count;
static unsigned int delays;

static ssize_t test_get_device_list(libusb_context *ctx, libusb_device ***list)
{
	(void)ctx;
	*list = device_list;
	return device_count;
}

static void test_free_device_list(libusb_device **list, int unref)
{
	(void)list;
	(void)unref;
}

static int test_get_descriptor(libusb_device *dev,
		struct libusb_device_descriptor *desc)
{
	*desc = ((struct fake_device *)dev)->desc;
	return 0;
}

static uint8_t test_get_bus(libusb_device *dev)
{
	return ((struct fake_device *)dev)->bus;
}

static uint8_t test_get_address(libusb_device *dev)
{
	return ((struct fake_device *)dev)->address;
}

static int test_get_ports(libusb_device *dev, uint8_t *ports, int length)
{
	assert(length >= 1);
	ports[0] = ((struct fake_device *)dev)->port;
	return 1;
}

static libusb_device *test_ref_device(libusb_device *dev)
{
	return dev;
}

static void test_unref_device(libusb_device *dev)
{
	(void)dev;
}

static int test_open(libusb_device *dev, libusb_device_handle **handle)
{
	*handle = (libusb_device_handle *)dev;
	return 0;
}

static void test_close(libusb_device_handle *handle)
{
	(void)handle;
}

static int test_get_string(libusb_device_handle *handle, uint8_t index,
		unsigned char *data, int length)
{
	struct fake_device *dev = (struct fake_device *)handle;

	return snprintf((char *)data, length, "%s",
		index == 1 ? dev->manufacturer : dev->product);
}

static int test_usleep(useconds_t usec)
{
	(void)usec;
	delays++;
	return 0;
}

#define libusb_get_device_list test_get_device_list
#define libusb_free_device_list test_free_device_list
#define libusb_get_device_descriptor test_get_descriptor
#define libusb_get_bus_number test_get_bus
#define libusb_get_device_address test_get_address
#define libusb_get_port_numbers test_get_ports
#define libusb_ref_device test_ref_device
#define libusb_unref_device test_unref_device
#define libusb_open test_open
#define libusb_close test_close
#define libusb_get_string_descriptor_ascii test_get_string
#define usleep test_usleep
#define main fx3lafw_load_main
#include "fx3lafw-load.c"
#undef main

static void setup_device(int index, uint16_t pid, uint16_t version,
		uint8_t device_class, const char *manufacturer, const char *product)
{
	fake[index] = (struct fake_device) {
		.desc = { .idVendor = FX3_VID, .idProduct = pid,
			.bcdDevice = version, .bDeviceClass = device_class,
			.iManufacturer = 1, .iProduct = 2 },
		.manufacturer = manufacturer, .product = product,
		.bus = 20, .address = (uint8_t)(index + 1),
		.port = (uint8_t)(index + 2),
	};
	device_list[index] = (libusb_device *)&fake[index];
}

int main(void)
{
	struct usb_path path = { .bus = 20, .ports = { 2 }, .depth = 1 };
	uint16_t vid, pid;

	assert(parse_usb_id("04b4:00F1", &vid, &pid) == 0);
	assert(vid == 0x04b4 && pid == 0x00f1);
	assert(parse_usb_id("04b4:6025", &vid, &pid) == 0 && pid == 0x6025);
	assert(parse_usb_id("04b4:00f3garbage", &vid, &pid) < 0);
	assert(parse_usb_id("-4b4:00f3", &vid, &pid) < 0);
	assert(parse_usb_id("4b4:f3", &vid, &pid) < 0);
	assert(parse_usb_id("04b4:gggg", &vid, &pid) < 0);
	assert(expected_product("/tmp/fx3lafw/cyfxbulksrcsink.img") == NULL);
	assert(!strcmp(expected_product("/tmp/fx3ulpifw-cypress-fx3.fw"), "fx3ulpifw"));
	assert(!strcmp(expected_product("fx3lafw-cypress-fx3.fw"), "fx3lafw"));

	device_count = 1;
	setup_device(0, FX3_PID, 0x0100, 0, "Cypress", "WestBridge");
	assert(wait_for_device(NULL, &path, FX3_VID, FX3_PID, "") == device_list[0]);
	assert(wait_for_device(NULL, &path, FX3_VID, FX3_PID, NULL) == NULL);
	assert(delays == RENUM_RETRIES);
	assert(wait_for_device(NULL, &path, FX3_VID, FX3_PID, "fx3lafw") == NULL);
	/* Product strings also reject a stale BootROM with a different descriptor. */
	fake[0].desc.bcdDevice = 1;
	assert(wait_for_device(NULL, &path, FX3_VID, FX3_PID, NULL) == NULL);

	setup_device(0, FX3_PID, 1, 0xff, "sigrok", "fx3lafw");
	assert(wait_for_device(NULL, &path, FX3_VID, FX3_PID, "fx3lafw") == device_list[0]);
	assert(wait_for_device(NULL, &path, FX3_VID, FX3_PID, "fx3ulpifw") == NULL);
	setup_device(0, FX3_PID, 1, 0xff, "sigrok", "fx3ulpifw");
	assert(wait_for_device(NULL, &path, FX3_VID, FX3_PID, "fx3ulpifw") == device_list[0]);

	/* The SDK changes PID; SS may also use a different companion-port path. */
	setup_device(0, 0x00f1, 0, 0, "Cypress", "BulkSrcSink");
	fake[0].bus = 21;
	fake[0].port = 7;
	assert(wait_for_device(NULL, &path, FX3_VID, 0x00f1, NULL) == device_list[0]);
	assert(wait_for_device(NULL, &path, FX3_VID, 0x6025, NULL) == NULL);
	setup_device(0, 0x6025, 0, 0, "Cypress", "HID");
	assert(wait_for_device(NULL, &path, FX3_VID, 0x6025, NULL) == device_list[0]);

	device_count = 2;
	setup_device(1, 0x6025, 0, 0, "Cypress", "HID");
	assert(find_device(NULL, FX3_VID, 0x6025, 0, 0, 0, NULL) == NULL);
	assert(find_device(NULL, FX3_VID, 0x6025, 1, 20, 2, NULL) == device_list[1]);
	puts("FX3 loader identity tests passed (no USB hardware accessed).");
	return 0;
}
