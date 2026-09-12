/*
 * Read the USB suspend/resume and link event counters from a loaded
 * fx3lafw device.
 *
 * These counters make the sleep/wake behaviour visible: a host that
 * suspends the port without ever resuming it shows up as a growing
 * suspend count with no matching resume, which is exactly the state that
 * used to leave the board powered but invisible on the bus.
 *
 * Copyright (C) 2026 Ruslan Migirov
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#include "../command.h"

#define FX3LAFW_VID 0x04b4
#define FX3LAFW_PID 0x00f3
#define USB_TIMEOUT_MS 1000

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device **devices = NULL;
	libusb_device_handle *handle = NULL;
	struct usb_events events;
	ssize_t count;
	int ret = 1;
	ssize_t i;

	if (argc > 1) {
		fprintf(stderr, "Usage: %s\n", argv[0]);
		return 2;
	}

	if (libusb_init(&ctx) != 0) {
		fprintf(stderr, "Unable to initialise libusb.\n");
		return 2;
	}

	count = libusb_get_device_list(ctx, &devices);
	for (i = 0; i < count; i++) {
		struct libusb_device_descriptor desc;
		unsigned char product[64];
		int product_len;

		if (libusb_get_device_descriptor(devices[i], &desc) != 0)
			continue;
		if (desc.idVendor != FX3LAFW_VID || desc.idProduct != FX3LAFW_PID)
			continue;
		if (libusb_open(devices[i], &handle) != 0) {
			handle = NULL;
			continue;
		}

		/* The ULPI analyzer image shares the vendor request numbers for
		 * a different command set, so only ask the analyzer image. */
		product_len = desc.iProduct ? libusb_get_string_descriptor_ascii(
			handle, desc.iProduct, product, sizeof(product) - 1) : 0;
		if (product_len <= 0 ||
				strcmp((char *)product, "fx3lafw") != 0) {
			libusb_close(handle);
			handle = NULL;
			continue;
		}
		break;
	}

	if (!handle) {
		fprintf(stderr, "No fx3lafw device found.\n");
		goto out;
	}

	memset(&events, 0, sizeof(events));
	if (libusb_control_transfer(handle,
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
				LIBUSB_ENDPOINT_IN,
			CMD_GET_USB_EVENTS, 0, 0,
			(unsigned char *)&events, sizeof(events),
			USB_TIMEOUT_MS) != (int)sizeof(events)) {
		fprintf(stderr, "Unable to read USB event counters.\n");
		goto out;
	}

	printf("suspend=%u resume=%u reset=%u link_down=%u link_up=%u "
		"setup=%u reconnect=%u vbus_present=%u link_reset=%u\n",
		events.suspend, events.resume, events.reset,
		events.link_down, events.link_up, events.setup,
		events.reconnect, events.vbus_present, events.link_reset);
	ret = 0;

out:
	if (handle)
		libusb_close(handle);
	if (devices)
		libusb_free_device_list(devices, 1);
	libusb_exit(ctx);
	return ret;
}
