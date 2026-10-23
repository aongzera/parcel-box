# Smart Parcel Box: product overview

> Embedded System Lab final project, Fall 2026. A 3-person team, with code written mostly using agentic AI.

This is the one-stop briefing for teammates and AI agents: what the Box is, how it behaves, how the parts link together, and how the work is organised. The words in **bold** have exact meanings defined in [GLOSSARY.md](GLOSSARY.md). Use those words and not their synonyms (say **Lid**, not door; **Collection**, not pickup).

Related docs:
- [Spec (aongzera/parcel-box#1)](https://github.com/aongzera/parcel-box/issues/1): the full user stories and implementation decisions
- [ADR 0001](docs/adr/0001-stm32-owns-box-state.md): the STM32 owns all Box state
- [ADR 0002](docs/adr/0002-firebase-over-netpie.md): Firebase, not NETPIE
- [AGENTS.md](AGENTS.md): agent setup and CI rules

---

## 1. What it is

An IoT parcel drop **Box**, built from a shoebox with a hinged **Lid**. It knows when a parcel has been delivered and when it has been collected, and it tells the **Owner** about both. If parcels are left outside, the Owner only finds out by going to look, and the parcels can get stolen or rained on. This Box fixes that.

## 2. Course requirements (fixed, not negotiable)

- The **STM32** reads and writes sensor data. The **ESP32-S3 (NodeMCU)** is its WiFi bridge.
- The application must include:
  1. Communication and some processing between the STM32 and the NodeMCU.
  2. At least 3 types of sensor and/or actuator.
  3. A connection to the internet and a cloud system.
- Deliverables:
  - A one-paragraph weekly progress report on MCV.
  - On demo day (after final exam week):
    - A **4-minute presentation**.
    - A **final report of at least 4 pages**, where each member writes at least 1 page about their own role.
    - The **source code in a public repo**, submitted as an archive plus the repo link.
- Deadline: **Sun 25 Oct 2026**, Final Project Initialization on MCV, with the parts answer `19, 29, 11`.

## 3. Hardware

| Kit no. | Part | Role in the Box |
|---|---|---|
| 19 | KY-021 mini magnetic reed module | Lid open/closed. The magnet is on the Lid, the switch on the rim. |
| 29 | KY-032 IR obstacle-avoidance sensor | **Parcel presence**. Mounted low inside, angled at the floor. |
| 11 | KY-012 active buzzer | Audible feedback. An active buzzer can only beep, not play tones. |
| — | NUCLEO-F411RE | Runs all the Box logic |
| — | ESP32-S3 (NodeMCU) | WiFi bridge to the cloud |
| — | Shoebox | The enclosure. Electronics sit outside the parcel area. |

Known limits of the KY-032:
- **Dark or black parcels** absorb IR and may not be detected.
- **Flat envelopes** may lie under the beam.
- Its range is set with the potentiometer on the board.

Always test with a dark parcel.

## 4. How the Box behaves

### Lid cycles

A **Lid cycle** is the Lid opening and then closing. After the Lid closes, the Box waits about 2 s for the reading to settle (so a hand inside doesn't count). Then it compares its state **before** the Lid opened with its state **after**:

| Before → After | Outcome (Event) | Telegram Alert? |
|---|---|---|
| Empty → Occupied | **Delivery** (`DELIVERED`) | Yes |
| Occupied → Empty | **Collection** (`COLLECTED`) | Yes |
| Occupied → Occupied | **Opened while occupied** (`OPENED_OCCUPIED`) | Yes |
| Empty → Empty | **Opened while empty** (`OPENED_EMPTY`) | No, it goes in the log only |

Opened while occupied exists because the IR sensor can only say "something is inside", not how many parcels. The Box can't tell whether a parcel was added or taken, so it reports that it was opened and doesn't guess.

Other rules:
- The reed switch is debounced (~50 ms), so a wobble counts as one Lid cycle.
- At power-up, the Box reads its starting state and emits **no** Event, so there are no false Alerts.

### Buzzer

- A single short beep when a Delivery is registered. This is the Courier's confirmation.
- A repeating beep if the Lid stays open longer than about 30 s, which stops as soon as it closes.

### Without WiFi

The Box keeps detecting Lid cycles and beeping locally. Events are queued and delivered, in order and with their original times, once the connection comes back.

## 5. Architecture

```
 ┌──────────── Box (shoebox) ────────────┐
 │                                        │
 │  KY-021 reed ─┐                        │
 │  KY-032 IR ───┼─▶ NUCLEO-F411RE        │
 │  KY-012 buzz ◀┘   (all Box logic)      │
 │                       │  UART text     │
 │                       ▼  lines + ACK   │
 │                   ESP32-S3             │
 │                   (bridge: queue,      │
 │                    timestamp, forward) │
 └───────────────────────┬────────────────┘
                         │ WiFi
          ┌──────────────┴──────────────┐
          ▼                             ▼
 Firebase Realtime DB             Telegram Bot API
 (Box status + Event log)         (Alerts to the team group)
          │
          ▼
 Dashboard (static page on Firebase Hosting, live, read-only)
```

### Who owns what (ADR 0001)

- **The STM32 is the brain.** It reads the sensors, drives the buzzer and decides every Lid cycle outcome. Inside it are:
  - the **Box logic core**: pure C with no HAL, and tested on a PC;
  - an **Event sender**: sequence numbers, an outbox, resend until ACK;
  - a thin **hardware adapter**: the only code that touches HAL.
- **The ESP32 is only a messenger.** It has no Box logic. It parses lines, replies ACK, ignores duplicates, stamps times (NTP), queues Events while offline, and writes to Firebase and Telegram.
- Don't move logic to the ESP32 just because it is more powerful. That breaks the course requirement, and the Box would stop working without WiFi.

### UART protocol (the contract between the boards)

- 115200 baud, 8N1, ASCII, one message per line ending in a newline. Fields are comma-separated, and unknown lines are ignored.

| Direction | Message | Meaning |
|---|---|---|
| STM32 → ESP32 | `EVT,<seq>,<OUTCOME>` | An Event. The outcome is one of the four names in section 4. |
| STM32 → ESP32 | `STS,<EMPTY\|OCCUPIED>,<LID_OPEN\|LID_CLOSED>` | **Box status**. Sent on every change and every few seconds as a heartbeat. |
| ESP32 → STM32 | `ACK,<seq>` | The Event was accepted. The STM32 resends until it gets this. |

The ESP32 ACKs a duplicate sequence number but ignores it, so each Event is delivered exactly once. A lower sequence number, or a heartbeat after a long silence, means the STM32 rebooted: start a new session and don't treat it as a duplicate.

**Why plain text matters:** either board can be tested alone by typing or reading these lines in a serial monitor. You can drive the ESP32 and Dashboard without an STM32 by typing `EVT,1,DELIVERED`.

### Cloud (ADR 0002)

- **Firebase Realtime Database** has one Box node:
  - **status:** Empty/Occupied, Lid open/closed, the last-Delivery time and the last-seen time;
  - **Event log:** a list of `{type, time or "unknown", seq}`.
- **Rules:** anyone can read; only the ESP32's device account can write.
- **Timestamps:** the ESP32 stamps each Event with the time it **arrived from the STM32**, not the upload time. If NTP has never synced, the time is "unknown". It must never show as 1970.
- **Dashboard:** a static page on Firebase Hosting with live updates, no login and no controls. It shows exactly four things:
  1. Empty/Occupied and Lid open/closed badges.
  2. The last-Delivery time.
  3. The Event log, newest first, including Opened while empty.
  4. Online/offline (offline if the last-seen time is more than ~30 s ago).
- **Telegram:** one bot, posting plain-language Alerts (e.g. "📦 Parcel delivered at 14:32") to the team's group chat, which acts as the Owner for the demo. LINE Notify was shut down in 2025, so we don't use LINE.

### Secrets (the repo is public)

- WiFi credentials, the Telegram bot token and chat ID, and the Firebase config and device login all go in **`esp32/include/secrets.h`**. That file is gitignored. Commit only **`secrets.example.h`** with placeholders.
- Never paste secrets into issues, PRs or the group chat. Send them privately.
- CI fails if any `secrets.h` is committed. If one leaks, **rotate the key**: deleting the file isn't enough, because it stays in git history.

## 6. Repo layout

| Path | What lives there |
|---|---|
| `stm32/` | The STM32CubeIDE project (NUCLEO-F411RE, HAL), including the Box logic core |
| `esp32/` | The PlatformIO project (Arduino framework) for the ESP32-S3 bridge |
| `cloud/` | The Dashboard page, Firebase rules and hosting config |
| `docs/` | ADRs, agent setup and the manual test checklists |
| `GLOSSARY.md` | The domain vocabulary. Read it before naming anything. |
| `AGENTS.md` | Instructions for AI agents, and the CI rules |
| `product.md` | This file |

The folders fill in as the tickets land.

## 7. Testing and CI

- **Automated, the only automated suite:** the Box logic core, compiled and run **on a PC** with `make test` from the repo root. Tests feed in sensor timelines and ticks, and check only what comes out (Events, Box status changes, buzzer commands). They never check internal variables.
- **Manual checklists** in `docs/`, written so non-coders can run them:
  - the ESP32 alone (typing UART lines);
  - the STM32 alone (reading its UART output);
  - the full system on the finished shoebox, including a dark parcel, a flat envelope and WiFi loss.
- **CI** (`.github/workflows/ci.yml`, on PRs and on pushes to `main`):
  - the secrets guard;
  - `make test`;
  - a PlatformIO build of `esp32/` with placeholder secrets.

  Jobs for parts that don't exist yet skip themselves.

## 8. How work is organised

- **Code tasks** are GitHub Issues: sub-issues of the spec, with GitHub's "blocked by" links.
  - `ready-for-agent` means a fully specified coding ticket, done with Claude Code.
  - `ready-for-human` means a hands-on or web-console ticket (wiring, building the Box, account setup), written as step-by-step instructions.
- **Overall progress and non-code tasks** (slides, reports, demo video) are tracked separately in the team's progress tracker.
- **Code** goes in through PRs to `main`, and CI must pass.

### Ticket map

| # | Ticket | Type | Blocked by |
|---|---|---|---|
| [2](https://github.com/aongzera/parcel-box/issues/2) | Wire the kit on a breadboard | human | none |
| [3](https://github.com/aongzera/parcel-box/issues/3) | Set up the Firebase project | human | none |
| [4](https://github.com/aongzera/parcel-box/issues/4) | Create the Telegram bot and team group | human | none |
| [5](https://github.com/aongzera/parcel-box/issues/5) | Box logic core with tests on the PC | agent | none |
| [6](https://github.com/aongzera/parcel-box/issues/6) | ESP32 + Dashboard tracer: live Box status | agent | 3 |
| [7](https://github.com/aongzera/parcel-box/issues/7) | STM32 firmware: real sensors to UART lines | agent | 2, 5 |
| [8](https://github.com/aongzera/parcel-box/issues/8) | Event log on the Dashboard | agent | 6 |
| [9](https://github.com/aongzera/parcel-box/issues/9) | Telegram Alerts | agent | 4, 8 |
| [10](https://github.com/aongzera/parcel-box/issues/10) | Connect the boards, with reliable delivery | agent | 7, 8 |
| [11](https://github.com/aongzera/parcel-box/issues/11) | Buzzer: Delivery beep and lid-left-open nag | agent | 7 |
| [12](https://github.com/aongzera/parcel-box/issues/12) | Shoebox build and IR tuning | human | 2 |
| [13](https://github.com/aongzera/parcel-box/issues/13) | Write the manual test checklists | agent | 8, 10 |
| [14](https://github.com/aongzera/parcel-box/issues/14) | Final end-to-end test run and backup demo video | human | 9–13 |

**Still open:** the pin map for the NUCLEO-F411RE must be written and posted on issue #2 before wiring starts.

## 9. Demo plan (4 minutes)

1. Show the Empty Box on the Dashboard.
2. A Courier drops a parcel in. The Box beeps, a Telegram Alert arrives, and the Dashboard flips to Occupied.
3. Open the Box again (Opened while occupied), which sends an Alert.
4. The Owner takes the parcel out (Collection). An Alert arrives, and the Dashboard goes back to Empty.
5. Optional: leave the Lid open to trigger the nag beep.

Keep a recorded backup video in case the room's WiFi fails.

## 10. Out of scope

- A lock or anything that physically secures the Box.
- Counting parcels.
- Multiple Boxes or Owners, logins, or controls on the Dashboard.
- Keeping queued Events through a power loss.
- An Alert for a Lid left open (the buzzer handles that).
- LINE, NETPIE, a mobile app, or battery power.

## 11. Rules for AI agents working here

1. Read `GLOSSARY.md`, the ADRs and this file first. Use glossary terms in code, tests, issues and commits.
2. Box logic belongs in the STM32's Box logic core, never in the ESP32 or the Dashboard (ADR 0001).
3. Keep the Box logic core free of HAL, so the PC tests keep working. `make test` must stay the entry point.
4. Don't change the UART protocol in section 5 without updating this file and both boards.
5. Never commit secrets. Every new secret gets a placeholder in `secrets.example.h`.
6. If a change contradicts an ADR, say so explicitly. Don't silently override it.
7. Tickets marked `ready-for-human` are for a person to do by hand. Don't try to automate console sign-ups or wiring.
