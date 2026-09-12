/*
 * Configure the USB3300 in passive mode and read back its ULPI registers.
 *
 * Usage: fx3lafw-ulpi-status [high|full|low|cached]
 *        [pins|mirror-pins|acquisition|phase=0..15|phase=off|delay=0..1023]
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#include "../command.h"

#define FX3LAFW_VID 0x04b4
#define FX3LAFW_PID 0x00f3
#define USB_TIMEOUT_MS 1000

static const char *speed_name(unsigned speed)
{
	switch (speed) {
	case 0:
		return "high";
	case 1:
		return "full";
	case 2:
		return "low";
	default:
		return "unknown";
	}
}

static int parse_speed(const char *name, uint16_t *speed)
{
	if (!strcmp(name, "high")) {
		*speed = 0;
		return 0;
	}
	if (!strcmp(name, "full")) {
		*speed = 1;
		return 0;
	}
	if (!strcmp(name, "low")) {
		*speed = 2;
		return 0;
	}
	return -1;
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *handle = NULL;
	struct ulpi_pin_status pin_status;
	struct ulpi_acquisition_status acquisition_status;
	struct ulpi_status status;
	uint16_t speed = 0;
	int configure = 1;
	int check_pins = 0;
	int mirror_pins = 0;
	int check_acquisition = 0;
	int phase = -1;
	uint16_t timing_config = ULPI_DLL_CONFIG_PHASE;
	int ret;
	int result = 1;

	if (argc > 3) {
		fprintf(stderr,
			"usage: %s [high|full|low|cached] "
			"[pins|mirror-pins|acquisition|phase=0..15|phase=off|"
			"delay=0..1023]\n", argv[0]);
		return 2;
	}
	if (argc == 2 && !strcmp(argv[1], "cached"))
		configure = 0;
	else if (argc == 2 && parse_speed(argv[1], &speed)) {
		fprintf(stderr, "unknown speed: %s\n", argv[1]);
		return 2;
	}
	if (argc == 3) {
		if (strcmp(argv[2], "pins") &&
		    strcmp(argv[2], "mirror-pins") &&
		    strcmp(argv[2], "acquisition") &&
		    strncmp(argv[2], "phase=", 6) &&
		    strncmp(argv[2], "delay=", 6)) {
			fprintf(stderr,
				"usage: %s [high|full|low|cached] "
				"[pins|mirror-pins|acquisition|phase=0..15|"
				"phase=off|delay=0..1023]\n", argv[0]);
			return 2;
		}
		if (!strcmp(argv[1], "cached"))
			configure = 0;
		else if (parse_speed(argv[1], &speed)) {
			fprintf(stderr,
				"usage: %s [high|full|low|cached] "
				"[pins|mirror-pins|acquisition|phase=0..15|"
				"phase=off|delay=0..1023]\n", argv[0]);
			return 2;
		}
		check_pins = !strcmp(argv[2], "pins") ||
			!strcmp(argv[2], "mirror-pins");
		mirror_pins = !strcmp(argv[2], "mirror-pins");
		check_acquisition = !strcmp(argv[2], "acquisition");
		if (!strcmp(argv[2], "phase=off")) {
			phase = ULPI_CORE_PHASE_DLL_OFF;
		} else if (!strncmp(argv[2], "phase=", 6) ||
			   !strncmp(argv[2], "delay=", 6)) {
			char *end;
			unsigned long parsed;
			bool fixed_delay = argv[2][0] == 'd';
			unsigned long maximum = fixed_delay ?
				ULPI_DLL_FIXED_DELAY_MAX : ULPI_CORE_PHASE_MAX;

			parsed = strtoul(argv[2] + 6, &end, 0);
			if (*end || parsed > maximum) {
				fprintf(stderr, "invalid ULPI DLL setting: %s\n",
					argv[2] + 6);
				return 2;
			}
			phase = (int)parsed;
			if (fixed_delay)
				timing_config = ULPI_DLL_CONFIG_FIXED_DELAY;
		}
	}

	ret = libusb_init(&ctx);
	if (ret) {
		fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(ret));
		return 1;
	}

	handle = libusb_open_device_with_vid_pid(ctx, FX3LAFW_VID, FX3LAFW_PID);
	if (!handle) {
		fprintf(stderr, "device not found or not openable\n");
		goto out;
	}

	if (configure) {
		ret = libusb_control_transfer(handle,
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
				LIBUSB_ENDPOINT_OUT,
			CMD_SET_ULPI_MODE, speed, ULPI_MODE_CONFIG_FORCE,
			NULL, 0, USB_TIMEOUT_MS);
		if (ret) {
			fprintf(stderr, "ULPI mode set failed: %d (%s)\n", ret,
				ret < 0 ? libusb_error_name(ret) : "unexpected data");
			goto out;
		}

	}

	if (phase >= 0) {
		ret = libusb_control_transfer(handle,
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
				LIBUSB_ENDPOINT_OUT,
			CMD_SET_ULPI_PHASE, (uint16_t)phase, timing_config,
			NULL, 0, USB_TIMEOUT_MS);
		if (ret) {
			fprintf(stderr, "ULPI phase set failed: %d (%s)\n", ret,
				ret < 0 ? libusb_error_name(ret) :
				"unexpected data");
			goto out;
		}
		if (phase == ULPI_CORE_PHASE_DLL_OFF)
			puts("next acquisition: PIB DLL disabled");
		else if (timing_config == ULPI_DLL_CONFIG_FIXED_DELAY)
			printf("next acquisition fixed DLL delay: %d\n", phase);
		else
			printf("next acquisition core phase: %d\n", phase);
		result = 0;
		goto out;
	}

	ret = libusb_control_transfer(handle,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
			LIBUSB_ENDPOINT_IN,
		CMD_GET_ULPI_STATUS, 0, 0, (unsigned char *)&status,
		sizeof(status), USB_TIMEOUT_MS);
	if (ret != (int)sizeof(status)) {
		fprintf(stderr, "ULPI status read failed: %d (%s)\n", ret,
			ret < 0 ? libusb_error_name(ret) : "short response");
		goto out;
	}

	printf("mode: %s (%u), FunctionControl expected 0x%02x\n",
	       speed_name(status.requested_speed), status.requested_speed,
	       status.expected_function_control);
	printf("identity: VID %02x%02x, PID %02x%02x\n",
	       status.vendor_id_high, status.vendor_id_low,
	       status.product_id_high, status.product_id_low);
	printf("registers: Function=0x%02x Interface=0x%02x "
	       "OTG=0x%02x Debug=0x%02x (LineState=%u)\n",
	       status.function_control, status.interface_control,
	       status.otg_control, status.debug, status.debug & 3);
	printf("scratch: before=0x%02x test=0x%02x read=0x%02x "
	       "restored=0x%02x\n",
	       status.scratch_before, status.scratch_expected,
	       status.scratch_after, status.scratch_restored);
	printf("checks: reads=0x%04x/0x%04x writes=0x%02x/0x%02x "
	       "matches=0x%02x/0x%02x\n",
	       status.read_ok_mask, ULPI_STATUS_READ_ALL,
	       status.write_ok_mask, ULPI_STATUS_WRITE_ALL,
	       status.match_mask, ULPI_STATUS_MATCH_ALL);
	printf("last GPIF: state=%u stat=%u status=0x%08x "
	       "gpio_invalue0=0x%08x alpha=0x%08x beta=0x%08x "
	       "data_ctrl=0x%08x\n",
	       status.last_gpif_state, status.last_gpif_stat,
	       status.last_gpif_status, status.gpio_invalue0,
	       status.alpha_stat, status.beta_stat, status.data_ctrl);
	printf("last write: state=%u stat=%u gpio_invalue0=0x%08x "
	       "alpha=0x%08x beta=0x%08x data_ctrl=0x%08x\n",
	       status.write_gpif_state, status.write_gpif_stat,
	       status.write_gpio_invalue0, status.write_alpha_stat,
	       status.write_beta_stat, status.write_data_ctrl);
	printf("last read ingress: 0x%08x "
	       "(D10:DIR=%u D9:NXT=%u D8:STP=%u data=0x%02x)\n",
	       status.read_ingress_data,
	       !!(status.read_ingress_data & (1U << 10)),
	       !!(status.read_ingress_data & (1U << 9)),
	       !!(status.read_ingress_data & (1U << 8)),
	       status.read_ingress_data & 0xff);
	printf("GPIO override: mask=0x%08x DQ8=0x%08x DQ9=0x%08x\n",
	       status.gpio_simple_override0, status.gpio8_config,
	       status.gpio9_config);

	if (check_pins) {
		ret = libusb_control_transfer(handle,
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
				LIBUSB_ENDPOINT_IN,
				CMD_GET_ULPI_PIN_STATUS, 0, mirror_pins,
			(unsigned char *)&pin_status, sizeof(pin_status),
			USB_TIMEOUT_MS);
		if (ret != (int)sizeof(pin_status)) {
			fprintf(stderr, "ULPI pin status read failed: %d (%s)\n",
				ret, ret < 0 ? libusb_error_name(ret) :
				"short response");
			goto out;
		}

			printf("pin samples (%s states): %u\n",
			       mirror_pins ? "mirror" : "explicit",
			       pin_status.samples);
		printf("  DIR: CTL2 high=%u transitions=%u; "
		       "DQ10 high=%u transitions=%u; mismatches=%u\n",
		       pin_status.high[ULPI_PIN_CTL2_DIR],
		       pin_status.transitions[ULPI_PIN_CTL2_DIR],
		       pin_status.high[ULPI_PIN_DQ10_DIR],
		       pin_status.transitions[ULPI_PIN_DQ10_DIR],
		       pin_status.mismatches[ULPI_PIN_PAIR_DIR]);
		printf("  NXT: CTL0 high=%u transitions=%u; "
		       "DQ9 high=%u transitions=%u; mismatches=%u\n",
		       pin_status.high[ULPI_PIN_CTL0_NXT],
		       pin_status.transitions[ULPI_PIN_CTL0_NXT],
		       pin_status.high[ULPI_PIN_DQ9_NXT],
		       pin_status.transitions[ULPI_PIN_DQ9_NXT],
		       pin_status.mismatches[ULPI_PIN_PAIR_NXT]);
		printf("  STP: CTL3 high=%u transitions=%u; "
		       "DQ8 high=%u transitions=%u; mismatches=%u\n",
		       pin_status.high[ULPI_PIN_CTL3_STP],
		       pin_status.transitions[ULPI_PIN_CTL3_STP],
		       pin_status.high[ULPI_PIN_DQ8_STP],
		       pin_status.transitions[ULPI_PIN_DQ8_STP],
		       pin_status.mismatches[ULPI_PIN_PAIR_STP]);
			printf("  old-layout check: DIR(CTL2)-DQ8 mismatches=%u; "
			       "NXT(CTL0)-DQ10 mismatches=%u\n",
			       pin_status.dir_to_dq8_mismatches,
			       pin_status.nxt_to_dq10_mismatches);
			printf("  DATA: nonzero=%u transitions=%u; "
			       "while DIR=1 samples=%u nonzero=%u NXT=1=%u "
			       "OR=0x%02x\n",
			       pin_status.data_nonzero_samples,
			       pin_status.data_transitions,
			       pin_status.rx_samples,
			       pin_status.rx_nonzero_samples,
			       pin_status.rx_nxt_samples,
			       pin_status.rx_data_or & 0xff);
			printf("  INGRESS: nonzero=%u transitions=%u; "
			       "while DIR=1 nonzero=%u OR=0x%02x\n",
			       pin_status.ingress_nonzero_samples,
			       pin_status.ingress_transitions,
			       pin_status.rx_ingress_nonzero_samples,
			       pin_status.rx_ingress_or & 0xff);
			printf("  GPIF: DRIVE=%u RECEIVE=%u other=%u "
			       "dq_oen=%u\n",
			       pin_status.drive_state_samples,
			       pin_status.receive_state_samples,
			       pin_status.other_state_samples,
			       pin_status.dq_oen_samples);
			}

		if (check_acquisition) {
			ret = libusb_control_transfer(handle,
				LIBUSB_REQUEST_TYPE_VENDOR |
					LIBUSB_RECIPIENT_DEVICE |
					LIBUSB_ENDPOINT_IN,
				CMD_GET_ULPI_ACQ_STATUS, 0, 0,
				(unsigned char *)&acquisition_status,
				sizeof(acquisition_status), USB_TIMEOUT_MS);
			if (ret != (int)sizeof(acquisition_status)) {
				fprintf(stderr,
					"ULPI acquisition status read failed: "
					"%d (%s)\n", ret,
					ret < 0 ? libusb_error_name(ret) :
					"short response");
				goto out;
			}

			printf("acquisition polls: total=%u DIR=1=%u "
			       "sampling=%u waiting=%u\n",
			       acquisition_status.poll_count,
			       acquisition_status.dir_high_count,
			       acquisition_status.sampling_state_count,
			       acquisition_status.wait_state_count);
			printf("  data: DIR/GPIO-nonzero=%u "
			       "DIR/ingress-nonzero=%u "
			       "alpha DQ_OEN while DIR=1=%u "
			       "ingress/nonzero=%u\n",
			       acquisition_status.dir_data_nonzero_count,
			       acquisition_status.dir_ingress_nonzero_count,
			       acquisition_status.alpha_dq_oen_while_dir_count,
			       acquisition_status.ingress_nonzero_count);
					printf("  config: BUS_CONFIG=0x%08x "
					       "BUS_CONFIG2=0x%08x AD_CONFIG=0x%08x "
					       "CTRL_DIRECTION=0x%08x "
					       "CTRL_POLARITY=0x%08x "
					       "sample word1 base=0x%08x mirror=0x%08x\n",
				       acquisition_status.bus_config,
				       acquisition_status.bus_config2,
				       acquisition_status.ad_config,
				       acquisition_status.ctrl_bus_direction,
				       acquisition_status.ctrl_bus_polarity,
				       acquisition_status.sample_base_word1,
				       acquisition_status.sample_mirror_word1);
				printf("  clocks: GPIF_CONFIG=0x%08x "
				       "PIB_DLL_CTRL=0x%08x %s=%u\n",
				       acquisition_status.gpif_config,
				       acquisition_status.pib_dll_ctrl,
				       acquisition_status.dll_config ==
					       ULPI_DLL_CONFIG_FIXED_DELAY ?
					       "fixed delay" : "core phase",
				       acquisition_status.core_phase);
				printf("  last: GPIO=0x%08x alpha=0x%08x "
				       "waveform=0x%08x ingress=0x%02x\n",
				       acquisition_status.last_gpio_invalue0,
				       acquisition_status.last_alpha_stat,
				       acquisition_status.last_waveform_ctrl_stat,
				       acquisition_status.last_ingress_data & 0xff);
			}

	if (status.read_ok_mask == ULPI_STATUS_READ_ALL &&
	    status.write_ok_mask == ULPI_STATUS_WRITE_ALL &&
	    status.match_mask == ULPI_STATUS_MATCH_ALL) {
		puts("ULPI register diagnostics: PASS");
		result = 0;
	} else {
		puts("ULPI register diagnostics: FAIL");
	}

out:
	if (handle)
		libusb_close(handle);
	libusb_exit(ctx);
	return result;
}
