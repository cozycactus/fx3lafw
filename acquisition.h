struct acquisition_status;

extern void start_acquisition(uint8_t bits, uint32_t delay,
			      uint16_t clock_divisor_x2,
			      uint8_t external_clock, uint8_t invert_clock);
extern void stop_acquisition(void);
extern void setup_acquisition(void);
extern void poll_acquisition(void);
extern void get_acquisition_status(volatile struct acquisition_status *status);
