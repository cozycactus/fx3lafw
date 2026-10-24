/*
 * Validate raw 16-bit fx3ulpifw captures without protocol-decoder overhead.
 *
 * Sample bits are D0-D7, STP, NXT, and DIR in bits 0 through 10.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACKET_CAPACITY 8192
#define DEFAULT_SOF_PERIOD 7500
/* Current 16-bit ULPI firmware/driver buffer geometry, for diagnostics only. */
#define DMA_BUFFER_SAMPLES 12288
#define HOST_TRANSFER_SAMPLES 131072

enum pid_kind {
	PID_UNKNOWN,
	PID_TOKEN,
	PID_DATA,
	PID_HANDSHAKE,
	PID_SPECIAL,
};

struct checker {
	uint8_t packet[PACKET_CAPACITY];
	size_t packet_len;
	uint64_t packet_start;
	uint64_t samples;
	uint64_t packets;
	uint64_t pid_count[256];
	uint64_t invalid_pid;
	uint64_t unknown_pid;
	uint64_t length_errors;
	uint64_t crc5_errors;
	uint64_t crc16_errors;
	uint64_t packet_overflows;
	uint64_t rxdata_without_active;
	uint64_t dir_ends;
	uint64_t phy_errors;
	uint64_t sof_count;
	uint64_t last_sof;
	uint64_t sof_intervals;
	uint64_t sof_period_errors;
	uint64_t sof_delta_min;
	uint64_t sof_delta_max;
	uint64_t expected_samples;
	unsigned expected_sof_period;
	bool one_cycle;
	bool verbose;
	bool have_expected_samples;
	bool previous_valid;
	bool previous_dir;
	bool rx_active;
	unsigned diagnostics;
};

static uint32_t reverse_bits(uint32_t value, unsigned count)
{
	uint32_t result = 0;
	unsigned bit;

	for (bit = 0; bit < count; bit++)
		result |= ((value >> bit) & 1U) << (count - bit - 1);
	return result;
}

static uint8_t calc_crc5(uint16_t value)
{
	uint32_t crc = 0x1f;
	unsigned bit;

	for (bit = 0; bit < 11; bit++) {
		uint32_t input = (value >> bit) & 1U;

		crc <<= 1;
		if (input != (crc >> 5))
			crc ^= 0x25;
		crc &= 0x1f;
	}
	return (uint8_t)reverse_bits(crc ^ 0x1f, 5);
}

static uint16_t calc_crc16(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffff;
	size_t byte;
	unsigned bit;

	for (byte = 0; byte < length; byte++) {
		for (bit = 0; bit < 8; bit++) {
			uint32_t input = (data[byte] >> bit) & 1U;

			crc <<= 1;
			if (input != (crc >> 16))
				crc ^= 0x18005;
			crc &= 0xffff;
		}
	}
	return (uint16_t)reverse_bits(crc ^ 0xffff, 16);
}

static bool valid_pid(uint8_t pid)
{
	return (pid >> 4) == ((~pid) & 0x0f);
}

static enum pid_kind pid_kind(uint8_t pid)
{
	switch (pid) {
	case 0xe1: /* OUT */
	case 0x69: /* IN */
	case 0xa5: /* SOF */
	case 0x2d: /* SETUP */
	case 0xb4: /* PING */
		return PID_TOKEN;
	case 0xc3: /* DATA0 */
	case 0x4b: /* DATA1 */
	case 0x87: /* DATA2 */
	case 0x0f: /* MDATA */
		return PID_DATA;
	case 0xd2: /* ACK */
	case 0x5a: /* NAK */
	case 0x1e: /* STALL */
	case 0x96: /* NYET */
		return PID_HANDSHAKE;
	case 0x3c: /* PRE/ERR */
	case 0x78: /* SPLIT */
		return PID_SPECIAL;
	default:
		return PID_UNKNOWN;
	}
}

static bool crc16_ok(const uint8_t *packet, size_t length)
{
	uint16_t received;

	if (length < 3)
		return false;
	received = packet[length - 2] | ((uint16_t)packet[length - 1] << 8);
	return calc_crc16(packet + 1, length - 3) == received;
}

static void correct_one_cycle_tail(struct checker *checker, uint8_t rxcmd)
{
	uint8_t pid;
	enum pid_kind kind;
	bool drop = false;

	if (!checker->one_cycle || !checker->packet_len)
		return;
	if (((checker->packet[checker->packet_len - 1] ^ rxcmd) & ~0x30) != 0)
		return;

	pid = checker->packet[0];
	if (!valid_pid(pid))
		return;
	kind = pid_kind(pid);
	if (kind == PID_TOKEN)
		drop = checker->packet_len == 4;
	else if (kind == PID_HANDSHAKE || pid == 0x3c)
		drop = checker->packet_len == 2;
	else if (kind == PID_DATA && checker->packet_len >= 4)
		drop = !crc16_ok(checker->packet, checker->packet_len) &&
			crc16_ok(checker->packet, checker->packet_len - 1);

	if (drop)
		checker->packet_len--;
}

static void record_sof(struct checker *checker)
{
	uint64_t delta;
	uint64_t minimum = checker->expected_sof_period - 1;
	uint64_t maximum = checker->expected_sof_period + 1;

	checker->sof_count++;
	if (checker->sof_count == 1) {
		checker->last_sof = checker->packet_start;
		return;
	}

	delta = checker->packet_start - checker->last_sof;
	checker->last_sof = checker->packet_start;
	checker->sof_intervals++;
	if (delta < checker->sof_delta_min)
		checker->sof_delta_min = delta;
	if (delta > checker->sof_delta_max)
		checker->sof_delta_max = delta;
	if (delta < minimum || delta > maximum)
		checker->sof_period_errors++;
}

static void report_packet_error(struct checker *checker, const char *kind)
{
	size_t index;

	if (!checker->verbose || checker->diagnostics >= 32)
		return;
	fprintf(stderr,
		"packet_error=%s start=%" PRIu64 " end=%" PRIu64
		" len=%zu pid=0x%02x mod12288=%" PRIu64
		" mod131072=%" PRIu64 " bytes=",
		kind, checker->packet_start, checker->samples,
		checker->packet_len, checker->packet[0],
		checker->packet_start % DMA_BUFFER_SAMPLES,
		checker->packet_start % HOST_TRANSFER_SAMPLES);
	for (index = 0; index < checker->packet_len && index < 8; index++)
		fprintf(stderr, "%s%02x", index ? "," : "",
			checker->packet[index]);
	if (checker->packet_len > 8)
		fputs(",...", stderr);
	fputc('\n', stderr);
	checker->diagnostics++;
}

static void finish_packet(struct checker *checker, uint8_t rxcmd,
			  bool have_rxcmd)
{
	uint8_t pid;
	enum pid_kind kind;

	if (!checker->packet_len)
		return;
	if (have_rxcmd)
		correct_one_cycle_tail(checker, rxcmd);
	if (!checker->packet_len)
		return;

	checker->packets++;
	pid = checker->packet[0];
	checker->pid_count[pid]++;
	if (!valid_pid(pid)) {
		checker->invalid_pid++;
		report_packet_error(checker, "invalid-pid");
		checker->packet_len = 0;
		return;
	}

	kind = pid_kind(pid);
	if (kind == PID_UNKNOWN) {
		checker->unknown_pid++;
		report_packet_error(checker, "unknown-pid");
	} else if (kind == PID_TOKEN) {
		uint16_t token;
		uint8_t received_crc;

		if (checker->packet_len != 3) {
			checker->length_errors++;
			report_packet_error(checker, "token-length");
		} else {
			token = checker->packet[1] |
				((uint16_t)checker->packet[2] << 8);
			received_crc = (token >> 11) & 0x1f;
			if (calc_crc5(token) != received_crc) {
				checker->crc5_errors++;
				report_packet_error(checker, "crc5");
			} else if (pid == 0xa5) {
				record_sof(checker);
			}
		}
	} else if (kind == PID_DATA) {
		if (checker->packet_len < 3) {
			checker->length_errors++;
			report_packet_error(checker, "data-length");
		} else if (!crc16_ok(checker->packet, checker->packet_len)) {
			checker->crc16_errors++;
			report_packet_error(checker, "crc16");
		}
	} else if (kind == PID_HANDSHAKE || pid == 0x3c) {
		if (checker->packet_len != 1) {
			checker->length_errors++;
			report_packet_error(checker, "handshake-length");
		}
	}

	checker->packet_len = 0;
}

static void append_packet_byte(struct checker *checker, uint8_t value,
			       uint64_t sample)
{
	if (!checker->packet_len)
		checker->packet_start = sample;
	if (checker->packet_len == PACKET_CAPACITY) {
		checker->packet_overflows++;
		checker->packet_len = 0;
		return;
	}
	checker->packet[checker->packet_len++] = value;
}

static void process_sample(struct checker *checker, uint16_t sample)
{
	uint8_t value = sample & 0xff;
	bool dir = !!(sample & (1U << 10));
	bool nxt = !!(sample & (1U << 9));

	if (checker->previous_valid && dir != checker->previous_dir) {
		if (dir) {
			if (nxt)
				checker->rx_active = true;
		} else {
			/* ULPI 1.1: DIR deassertion also ends a receive packet. */
			if (checker->packet_len)
				checker->dir_ends++;
			finish_packet(checker, 0, false);
			checker->rx_active = false;
		}
		checker->previous_dir = dir;
		checker->samples++;
		return;
	}

	checker->previous_valid = true;
	checker->previous_dir = dir;
	if (dir) {
		if (nxt) {
			if (!checker->rx_active)
				checker->rxdata_without_active++;
			append_packet_byte(checker, value, checker->samples);
		} else {
			unsigned event;
			bool active;

			event = (value >> 4) & 3U;
			active = event == 1 || event == 3;
			/* RXCMD may interrupt data while RxActive remains asserted. */
			if (!active)
				finish_packet(checker, value, true);
			checker->rx_active = active;
			if (event == 3)
				checker->phy_errors++;
		}
	} else if (checker->rx_active) {
		if (checker->packet_len)
			checker->dir_ends++;
		finish_packet(checker, 0, false);
		checker->rx_active = false;
	}
	checker->samples++;
}

static int check_file(struct checker *checker, const char *path)
{
	uint8_t buffer[1024 * 1024];
	FILE *file;
	size_t length;

	file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 2;
	}
	while ((length = fread(buffer, 1, sizeof(buffer), file)) != 0) {
		size_t offset;

		if (length & 1) {
			fprintf(stderr, "%s: odd-sized raw sample block\n", path);
			fclose(file);
			return 2;
		}
		for (offset = 0; offset < length; offset += 2) {
			uint16_t sample = buffer[offset] |
				((uint16_t)buffer[offset + 1] << 8);

			process_sample(checker, sample);
		}
	}
	if (ferror(file)) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		fclose(file);
		return 2;
	}
	fclose(file);
	finish_packet(checker, 0, false);
	return 0;
}

static void print_pid(const struct checker *checker, uint8_t pid,
		      const char *name)
{
	printf(" %s=%" PRIu64, name, checker->pid_count[pid]);
}

static bool parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno || *text == '-' || end == text || *end)
		return false;
	*value = parsed;
	return true;
}

int main(int argc, char **argv)
{
	struct checker checker = {
		.expected_sof_period = DEFAULT_SOF_PERIOD,
		.sof_delta_min = UINT64_MAX,
	};
	uint64_t errors;
	uint64_t coverage_errors;
	int arg = 1;
	int ret;

	while (arg < argc && !strncmp(argv[arg], "--", 2)) {
		if (!strcmp(argv[arg], "--nxt-one-cycle"))
			checker.one_cycle = true;
		else if (!strcmp(argv[arg], "--verbose"))
			checker.verbose = true;
		else if (!strcmp(argv[arg], "--expect-samples")) {
			if (++arg == argc ||
			    !parse_u64(argv[arg], &checker.expected_samples)) {
				fprintf(stderr, "invalid expected sample count\n");
				return 2;
			}
			checker.have_expected_samples = true;
		}
		else
			break;
		arg++;
	}
	if (argc - arg < 1 || argc - arg > 2) {
		fprintf(stderr,
			"usage: %s [--nxt-one-cycle] [--verbose] "
			"[--expect-samples COUNT] "
			"FILE [SOF_PERIOD]\n",
			argv[0]);
		return 2;
	}
	if (argc - arg == 2) {
		char *end;
		unsigned long value = strtoul(argv[arg + 1], &end, 0);

		if (*end || value < 2 || value > UINT32_MAX) {
			fprintf(stderr, "invalid SOF period: %s\n", argv[arg + 1]);
			return 2;
		}
		checker.expected_sof_period = (unsigned)value;
	}

	ret = check_file(&checker, argv[arg]);
	if (ret)
		return ret;

	printf("samples=%" PRIu64 " packets=%" PRIu64,
	       checker.samples, checker.packets);
	print_pid(&checker, 0xa5, "SOF");
	print_pid(&checker, 0xe1, "OUT");
	print_pid(&checker, 0x69, "IN");
	print_pid(&checker, 0x2d, "SETUP");
	print_pid(&checker, 0xc3, "DATA0");
	print_pid(&checker, 0x4b, "DATA1");
	print_pid(&checker, 0x87, "DATA2");
	print_pid(&checker, 0x0f, "MDATA");
	print_pid(&checker, 0xd2, "ACK");
	print_pid(&checker, 0x5a, "NAK");
	print_pid(&checker, 0x1e, "STALL");
	print_pid(&checker, 0x96, "NYET");
	putchar('\n');

	printf("errors: invalid_pid=%" PRIu64 " unknown_pid=%" PRIu64
	       " length=%" PRIu64 " crc5=%" PRIu64 " crc16=%" PRIu64
	       " overflow=%" PRIu64 " rx_without_active=%" PRIu64
	       " phy=%" PRIu64 "\n",
	       checker.invalid_pid, checker.unknown_pid,
	       checker.length_errors, checker.crc5_errors,
	       checker.crc16_errors, checker.packet_overflows,
	       checker.rxdata_without_active, checker.phy_errors);
	printf("packet_end: DIR=%" PRIu64 "\n", checker.dir_ends);
	if (checker.sof_intervals) {
		printf("sof: valid=%" PRIu64 " intervals=%" PRIu64
		       " delta_min=%" PRIu64 " delta_max=%" PRIu64
		       " period_errors=%" PRIu64 "\n",
		       checker.sof_count, checker.sof_intervals,
		       checker.sof_delta_min, checker.sof_delta_max,
		       checker.sof_period_errors);
	} else {
		printf("sof: valid=%" PRIu64 " intervals=0\n",
		       checker.sof_count);
	}

	coverage_errors = (checker.packets == 0) + (checker.sof_count < 2) +
		(checker.have_expected_samples &&
		 checker.samples != checker.expected_samples);
	printf("coverage: packets>=1=%s SOF>=2=%s",
	       checker.packets ? "yes" : "no",
	       checker.sof_count >= 2 ? "yes" : "no");
	if (checker.have_expected_samples)
		printf(" samples==%" PRIu64 "=%s",
		       checker.expected_samples,
		       checker.samples == checker.expected_samples ? "yes" : "no");
	putchar('\n');
	errors = checker.invalid_pid + checker.unknown_pid +
		checker.length_errors + checker.crc5_errors +
		checker.crc16_errors + checker.packet_overflows +
		checker.rxdata_without_active +
		checker.phy_errors + checker.sof_period_errors + coverage_errors;
	printf("result: %s (%" PRIu64 " errors)\n",
	       errors ? "FAIL" : "PASS", errors);
	return errors ? 1 : 0;
}
