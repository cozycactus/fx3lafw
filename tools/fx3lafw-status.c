/*
 * Poll CMD_GET_ACQ_STATUS repeatedly and print a time series of the
 * GPIF state and DMA socket counters, to observe how a capture
 * progresses (or wedges) in real time.
 *
 * Usage: fx3lafw-status [interval_ms] [max_polls]
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <libusb.h>

#include "../command.h"

#define FX3LAFW_VID 0x04b4
#define FX3LAFW_PID 0x00f3
#define USB_TIMEOUT_MS 1000

static double now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *handle = NULL;
	struct acquisition_status st;
	int interval_ms = 5;
	int max_polls = 2000;
	int ret, i;
	double t0;

	if (argc > 1)
		interval_ms = atoi(argv[1]);
	if (argc > 2)
		max_polls = atoi(argv[2]);

	if (libusb_init(&ctx)) {
		fprintf(stderr, "libusb_init failed\n");
		return 1;
	}

	handle = libusb_open_device_with_vid_pid(ctx, FX3LAFW_VID, FX3LAFW_PID);
	if (!handle) {
		fprintf(stderr, "device not found or not openable\n");
		return 1;
	}

	t0 = now_ms();
	for (i = 0; i < max_polls; i++) {
		ret = libusb_control_transfer(handle,
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
				LIBUSB_ENDPOINT_IN,
			CMD_GET_ACQ_STATUS, 0, 0,
			(unsigned char *)&st, sizeof(st), USB_TIMEOUT_MS);
		if (ret != (int)sizeof(st)) {
			fprintf(stderr, "status read failed: %d (%s)\n", ret,
				ret < 0 ? libusb_error_name(ret) : "short");
			usleep(interval_ms * 1000);
			continue;
		}
		printf("%9.2f stat=%u state=%2u err=0x%04x intr=0x%08x "
		       "p0=%7u(%3u) p1=%7u(%3u) u2=%8u(%3u) "
		       "p0st=%08x p1st=%08x u2st=%08x pause=%u\n",
		       now_ms() - t0,
		       st.gpif_stat, st.gpif_state,
		       st.pib_error, st.pib_intr,
		       st.pib_sck0_count, st.pib_sck0_count / 24576,
		       st.pib_sck1_count, st.pib_sck1_count / 24576,
		       st.uib_sck2_count, st.uib_sck2_count / 24576,
		       st.pib_sck0_status, st.pib_sck1_status,
		       st.uib_sck2_status, st.pause_count);
		fflush(stdout);
		if (st.pib_error)
			break;
		usleep(interval_ms * 1000);
	}

	libusb_close(handle);
	libusb_exit(ctx);
	return 0;
}
