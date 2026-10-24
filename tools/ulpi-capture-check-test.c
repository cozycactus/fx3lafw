#include <assert.h>

#define main ulpi_capture_check_main
#include "ulpi-capture-check.c"
#undef main

static void feed(struct checker *checker, const uint16_t *samples, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		process_sample(checker, samples[i]);
}

static void assert_clean(const struct checker *checker)
{
	assert(checker->packets == 1);
	assert(checker->packet_len == 0);
	assert(!checker->invalid_pid && !checker->unknown_pid);
	assert(!checker->length_errors && !checker->crc5_errors);
	assert(!checker->crc16_errors && !checker->packet_overflows);
	assert(!checker->rxdata_without_active);
	assert(!checker->phy_errors);
}

int main(void)
{
	struct checker checker = {0};
	/* Recorded SOF with an RxActive RXCMD inserted after the PID. */
	const uint16_t sof[] = {
		0, 0x0600, 0x06a5, 0x045d, 0x06fe, 0x0632, 0x044d, 0,
	};
	/* CRC-16/USB check vector "123456789": 0xb4c8, little endian. */
	const uint16_t data[] = {
		0, 0x0600, 0x06c3, 0x0631, 0x045d, 0x0632, 0x0633,
		0x0634, 0x045d, 0x045d, 0x0635, 0x0636, 0x0637,
		0x0638, 0x0639, 0x06c8, 0x045d, 0x06b4, 0x044d, 0,
	};
	uint16_t damaged[sizeof(data) / sizeof(data[0])];
	const uint16_t tail[] = {
		0, 0x0600, 0x06a5, 0x06fe, 0x0632, 0x064d, 0x044d, 0,
	};
	const uint16_t phy_error[] = {
		0, 0x0600, 0x06a5, 0x047d, 0x06fe, 0x0632, 0x044d, 0,
	};
	const uint16_t dir_end[] = {0, 0x0600, 0x06d2, 0};
	const uint16_t truncated[] = {0, 0x0600, 0x06a5, 0};

	feed(&checker, sof, sizeof(sof) / sizeof(sof[0]));
	assert_clean(&checker);
	assert(checker.sof_count == 1 && checker.packet_start == 2);

	checker = (struct checker){0};
	feed(&checker, data, sizeof(data) / sizeof(data[0]));
	assert_clean(&checker);
	assert(checker.pid_count[0xc3] == 1);

	checker = (struct checker){0};
	memcpy(damaged, data, sizeof(data));
	damaged[3] ^= 1;
	feed(&checker, damaged, sizeof(damaged) / sizeof(damaged[0]));
	assert(checker.crc16_errors == 1 && checker.packets == 1);

	checker = (struct checker){0};
	feed(&checker, tail, sizeof(tail) / sizeof(tail[0]));
	assert(checker.length_errors == 1 && checker.sof_count == 0);

	checker = (struct checker){0};
	feed(&checker, phy_error, sizeof(phy_error) / sizeof(phy_error[0]));
	assert(checker.phy_errors == 1 && checker.packets == 1);
	assert(checker.sof_count == 1 && checker.length_errors == 0);

	checker = (struct checker){0};
	feed(&checker, dir_end, sizeof(dir_end) / sizeof(dir_end[0]));
	assert_clean(&checker);
	assert(checker.dir_ends == 1);

	checker = (struct checker){0};
	feed(&checker, truncated, sizeof(truncated) / sizeof(truncated[0]));
	assert(checker.dir_ends == 1 && checker.length_errors == 1);
	puts("ULPI capture checker: 7 regression cases passed.");
	return 0;
}
