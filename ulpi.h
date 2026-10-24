#ifndef ULPI_H
#define ULPI_H

#include <stdbool.h>
#include <stdint.h>

enum ulpi_speed {
  ULPI_SPEED_HIGH = 0,
  ULPI_SPEED_FULL = 1,
  ULPI_SPEED_LOW = 2,
};

struct ulpi_status;
struct ulpi_pin_status;

void ulpi_init(void);
bool ulpi_configure_passive(enum ulpi_speed speed);
bool ulpi_force_configure_passive(enum ulpi_speed speed);
void ulpi_start_idle(void);
void ulpi_get_status(volatile struct ulpi_status *status);
void ulpi_get_pin_status(volatile struct ulpi_pin_status *status,
			 bool mirror_states);

#endif
