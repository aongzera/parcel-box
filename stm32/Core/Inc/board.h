/*
 * Board I/O: the sensor and buzzer pins from docs/pinmap.md. Part of the
 * hardware adapter, so it may use HAL. The pins themselves are set up by
 * MX_GPIO_Init() in main.c, from parcel-box.ioc (LID_REED, PARCEL_IR and
 * BUZZER, which starts LOW, i.e. silent).
 */
#ifndef BOARD_H
#define BOARD_H

#include "box_core.h"

/* Read both sensors and say what they mean. No filtering: the Box logic
 * core debounces. */
BoxReadings board_read_sensors(void);

/* Turn the KY-012 active buzzer on or off. The Box logic core decides when
 * (its on_buzzer output); this only drives the pin. */
void board_set_buzzer(bool on);

#endif /* BOARD_H */
