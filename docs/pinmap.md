# Pin map

This is the wiring contract between the hardware and the firmware. **Wire exactly this.** If you need to change a pin, change it here first and tell whoever owns the firmware.

Everything runs on **3.3 V logic**. Power the modules from the Nucleo's **3V3** pin, not 5V. The STM32 and the ESP32-S3 are both 3.3 V chips, so 5 V signals could damage them.

## NUCLEO-F411RE

| Module | Module pin | Nucleo header label | STM32 pin | Firmware setting |
|---|---|---|---|---|
| KY-021 reed switch | `S` (signal) | **D4** | PB5 | GPIO input, pull-up |
| | `+` (middle) | 3V3 | — | — |
| | `-` | GND | — | — |
| KY-032 IR obstacle | `out` | **D5** | PB4 | GPIO input, pull-up |
| | `+` | 3V3 | — | — |
| | `GND` | GND | — | — |
| | `EN` | leave unconnected; keep the **EN jumper ON** | — | — |
| KY-012 active buzzer | `S` (signal) | **D6** | PB10 | GPIO output, push-pull |
| | `-` | GND | — | — |
| Link to ESP32 (TX) | — | **D8** | PA9 | USART1_TX, 115200 8N1 |
| Link to ESP32 (RX) | — | **D2** | PA10 | USART1_RX, 115200 8N1 |
| Debug log to PC | — | (USB cable, no wiring) | PA2 / PA3 | USART2, the ST-LINK virtual COM port, 115200 8N1 |

The "D" labels are the Arduino-style labels printed next to the Nucleo's headers.

## ESP32-S3

| Signal | ESP32-S3 pin | Connect to |
|---|---|---|
| UART1 RX | **GPIO18** | Nucleo **D8** (PA9, TX) |
| UART1 TX | **GPIO17** | Nucleo **D2** (PA10, RX) |
| GND | any **GND** | Nucleo **GND** (the boards **must** share ground) |

The ESP32's own USB port (the one used for flashing) stays free for its serial monitor.

## Signal meanings (what the firmware should expect)

| Signal | Reading | Meaning |
|---|---|---|
| Reed (PB5) | LOW | The magnet is near the switch, so the **Lid is closed** |
| Reed (PB5) | HIGH | The **Lid is open** |
| IR (PB4) | LOW | Something is in front of the sensor, so **Parcel presence is true** |
| IR (PB4) | HIGH | Nothing is detected |
| Buzzer (PB10) | HIGH | Beep |

Check these with the real modules before trusting them, as some KY clones are inverted. Use the sensor's on-board LED and a multimeter, or the firmware's debug log. If a reading is inverted, fix it in the hardware adapter, **not** in the Box logic core.

## Why these pins

- **D4, D5 and D6 are plain GPIO.** They are not used by anything else on the Nucleo, so they are safe to use.
- These pins are reserved and must be avoided:
  - PA5 / D13: the green user LED LD2.
  - PC13: the blue user button.
  - PA13 / PA14: SWD, which you need for flashing.
  - PA2 / PA3: the ST-LINK USB serial port.
- **USART1 (D8/D2) talks to the ESP32. USART2 (over USB) is a debug mirror.** The STM32 also prints every line it sends and receives to USART2, so you can watch the protocol on your laptop without an extra USB-serial adapter. This also covers testing the STM32 alone (ticket #7).
- **GPIO17/18** are the ESP32-S3's usual UART1 pins and are broken out on DevKit-style boards. If your board's labels differ, pick two free GPIOs and update this file.

## Known risks

- **Buzzer current:** the KY-012 can draw close to the STM32 pin limit (about 25 mA). At 3.3 V it is quieter but usually fine. If it is too quiet, or the board misbehaves while it beeps, drive it through an NPN transistor (e.g. 2N2222 with a 1 kΩ base resistor) instead of straight from the pin.
- **IR range at 3.3 V** is shorter than at 5 V. Tune the potentiometer with the sensor mounted in its final place (ticket #12).
- **Crossed UART:** TX always goes to RX. If nothing arrives, swap the two wires first.

## Wiring diagram

```
NUCLEO-F411RE                         KY-021 reed     KY-032 IR      KY-012 buzzer
  3V3 ─────────────────────┬──────────── +  ─────────── +
                           │
  GND ───────┬─────────────┼──────────── -  ─────────── GND ───────── -
             │             │
  D4 (PB5) ──┼─────────────┼──────────── S
  D5 (PB4) ──┼─────────────┼─────────────────────────── out
  D6 (PB10) ─┼─────────────┼──────────────────────────────────────── S
             │
  D8 (PA9)  ─┼──── TX ─▶ RX ──── GPIO18 ┐
  D2 (PA10) ─┼──── RX ◀─ TX ──── GPIO17 ├ ESP32-S3
             └──────── GND ───── GND    ┘
```
