# Smart Parcel Box

An IoT drop box that knows when a parcel has been delivered and when it has been collected, and tells the owner about both.

## Language

### The box

**Box**:
The physical parcel drop box, with its sensors, buzzer and both microcontrollers.
_Avoid_: Device, unit, locker

**Lid**:
The single opening of the Box, through which parcels are put in and taken out.
_Avoid_: Door, flap, hatch

**Parcel presence**:
Whether at least one parcel is currently inside the Box. It does not count parcels.
_Avoid_: Item detected, obstacle

### People

**Owner**:
The person the Box belongs to, who receives alerts and collects parcels.
_Avoid_: User, customer, recipient

**Courier**:
Anyone who puts a parcel into the Box.
_Avoid_: Delivery man, driver, sender

### Lifecycle

**Empty** / **Occupied**:
The two states of a Box: Occupied means Parcel presence is true, Empty means it is false.

**Box status**:
A snapshot of the Box right now: Empty or Occupied, and whether the Lid is open or closed. Unlike an Event, it describes the present rather than something that happened.
_Avoid_: State update, telemetry

**Lid cycle**:
One opening of the Lid followed by its closing. Each Lid cycle is judged by comparing the Box's state before it with the state after it.
_Avoid_: Open event, access

**Delivery**:
A Lid cycle that takes the Box from Empty to Occupied.
_Avoid_: Drop, arrival

**Collection**:
A Lid cycle that takes the Box from Occupied to Empty.
_Avoid_: Pickup, retrieval

**Opened while occupied**:
A Lid cycle that starts and ends Occupied: a parcel may have been added or taken, and the Box cannot tell which.

**Opened while empty**:
A Lid cycle that starts and ends Empty: someone looked inside and nothing changed.

**Event**:
A notable change in the Box, such as a Delivery or a Collection, that is reported to the cloud.
_Avoid_: Message, signal, notification

**Alert**:
A push message sent to the Owner's phone about an Event. Every Event except Opened while empty raises an Alert.
_Avoid_: Notification, notify

**Event log**:
The time-ordered history of every Event a Box has reported.
_Avoid_: History, feed

**Dashboard**:
The web page where the Owner sees the Box's current state and its Event log.
_Avoid_: Web app, UI, freeboard
