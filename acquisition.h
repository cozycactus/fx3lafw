struct acquisition_status;
#ifdef FX3_ULPI_SNIFFER
struct ulpi_acquisition_status;
#endif

extern void start_acquisition(uint8_t bits, uint32_t delay,
			      uint16_t clock_divisor_x2,
			      uint8_t external_clock, uint8_t invert_clock);
extern void stop_acquisition(void);
extern void setup_acquisition(void);
extern void poll_acquisition(void);
extern void get_acquisition_status(volatile struct acquisition_status *status);
#ifdef FX3_ULPI_SNIFFER
extern void get_ulpi_acquisition_status(
	volatile struct ulpi_acquisition_status *status);
extern void set_ulpi_acquisition_timing(uint16_t value, uint8_t config);
#endif
