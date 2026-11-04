/* Board I/O: see board.h and docs/pinmap.md. */
#include "board.h"

#include "main.h"   /* pin labels from parcel-box.ioc */

/*
 * The one place pin levels become meanings (docs/pinmap.md, "Signal
 * meanings"). If a KY clone turns out to be inverted, flip it here and
 * nowhere else:
 *   reed LOW = magnet near = Lid closed, so HIGH means the Lid is open;
 *   IR   LOW = something in front = Parcel presence.
 */
#define LID_OPEN_LEVEL       GPIO_PIN_SET
#define PARCEL_PRESENT_LEVEL GPIO_PIN_RESET

BoxReadings board_read_sensors(void)
{
    BoxReadings readings;
    readings.lid_open =
        HAL_GPIO_ReadPin(LID_REED_GPIO_Port, LID_REED_Pin) == LID_OPEN_LEVEL;
    readings.parcel_present =
        HAL_GPIO_ReadPin(PARCEL_IR_GPIO_Port, PARCEL_IR_Pin) == PARCEL_PRESENT_LEVEL;
    return readings;
}
