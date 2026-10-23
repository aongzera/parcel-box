# The STM32 owns all Box state; the ESP32 is only a bridge

The course requires the STM32 to read sensors and do processing, with the ESP32-S3 acting as the WiFi bridge. So the STM32 reads the sensors, drives the buzzer, and decides on Lid cycles (Delivery, Collection and so on) without any help from the network. It sends finished Events to the ESP32 over UART as newline-terminated ASCII lines (e.g. `EVT,DELIVERED\n`). The ESP32 queues each Event, forwards it to the cloud and replies `ACK`. The STM32 keeps resending until it gets that `ACK`.

Do not move logic onto the ESP32 just because it has more CPU. Doing so would break the course requirement, and the box would stop working whenever WiFi is down.
