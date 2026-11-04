/*
 * Board I/O: the sensor pins from docs/pinmap.md. Part of the hardware
 * adapter, so it may use HAL. The pins themselves are set up by
 * MX_GPIO_Init() in main.c, from parcel-box.ioc (LID_REED, PARCEL_IR and
 * BUZZER, which starts LOW and is driven by ticket #11).
 */
#ifndef BOARD_H
#define BOARD_H

#include "box_core.h"

/* Read both sensors and say what they mean. No filtering: the Box logic
 * core debounces. */
BoxReadings board_read_sensors(void);

#endif /* BOARD_H */
