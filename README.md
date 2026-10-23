# parcel-box

The Smart Parcel Box: an Embedded System Lab final project (STM32 + ESP32-S3) that detects parcel Deliveries and Collections and alerts the Owner.

**New here? Read [product.md](product.md) first.** It explains what the Box does and how the parts fit together. Then use this README to get set up.

| Doc | What it's for |
|---|---|
| [product.md](product.md) | The project overview, architecture and UART protocol |
| [GLOSSARY.md](GLOSSARY.md) | The exact meaning of words like Lid cycle, Delivery and Collection |
| [docs/pinmap.md](docs/pinmap.md) | What wire goes where |
| [docs/adr/](docs/adr/) | Why we made the big decisions |
| [AGENTS.md](AGENTS.md) | Instructions for AI coding agents |
| [Issues](https://github.com/aongzera/parcel-box/issues) | The task list; the spec is issue #1 |

---

## 1. Get the code

Install [Git](https://git-scm.com/downloads), then:

```bash
git clone https://github.com/aongzera/parcel-box.git
cd parcel-box
```

Before starting work each day, get the latest:

```bash
git pull
```

## 2. What's in the repo

```
parcel-box/
├── stm32/      STM32CubeIDE project for the NUCLEO-F411RE (all the Box logic)
├── esp32/      PlatformIO project for the ESP32-S3 (WiFi bridge)
├── cloud/      Dashboard web page + Firebase rules
├── docs/       ADRs, pin map, test checklists
└── Makefile    `make test` runs the Box logic tests on your PC
```

> Some folders don't exist yet. Each one is created by the ticket that builds it: `stm32/` by #5 and #7, `esp32/` and `cloud/` by #6.

## 3. Install the tools

You only need the tools for the part you're working on.

| You're working on | Install |
|---|---|
| STM32 firmware | [STM32CubeIDE](https://www.st.com/en/development-tools/stm32cubeide.html) (free; needs an ST account to download) |
| ESP32 firmware | [VS Code](https://code.visualstudio.com/) + the **PlatformIO IDE** extension. Antigravity also works, since it is based on VS Code. |
| Dashboard / Firebase | [Node.js LTS](https://nodejs.org/), then `npm install -g firebase-tools` |
| Running the PC tests | `gcc` and `make` (see section 7) |
| Watching serial output | Built into PlatformIO, or [PuTTY](https://www.putty.org/) / Tera Term |

### Why PlatformIO and not the Arduino IDE?

The ESP32 code **uses the Arduino framework**: the same `setup()`/`loop()`, `Serial`, `WiFi` and so on. We just build it with PlatformIO instead of the Arduino IDE, because:
- the board settings and exact library versions are saved in the repo (`esp32/platformio.ini`), so it builds the same way on every laptop;
- the Arduino IDE keeps those settings on each person's computer, which leads to "works on my machine" problems.

So the code will look familiar if you know Arduino. Only the buttons are different (see section 5).

## 4. Working on the STM32 (STM32CubeIDE)

### Open the project

1. Open STM32CubeIDE and pick any workspace folder, e.g. `C:\stm32-workspace`. **Don't** pick the repo folder itself.
2. Go to **File → Import… → General → Existing Projects into Workspace → Next**.
3. For **Select root directory**, browse to `parcel-box/stm32`.
4. The project appears in the list. **Untick "Copy projects into workspace"**, so you edit the files in the repo, then click **Finish**.

### Build, flash and run

1. Plug the Nucleo into your laptop with its USB cable.
2. **Build:** click the 🔨 hammer button (or press `Ctrl+B`).
3. **Flash and run:** click the ▶ green Run button. The first time, accept the default "STM32 C/C++ Application" settings. If it asks to update the ST-LINK firmware, say yes.
4. **Debug:** click the 🐞 bug button to step through code and set breakpoints.

### See the debug log

The STM32 prints everything it sends to the ESP32 over the USB cable too (see the [pin map](docs/pinmap.md)).
1. Find the COM port: Windows Device Manager → Ports → **STMicroelectronics STLink Virtual COM Port (COMx)**.
2. Open it at **115200** baud in PuTTY or Tera Term. In CubeIDE you can use **Window → Show View → Terminal**, then open a Serial terminal.
3. Open and close the Lid (move the magnet). You'll see lines like `STS,EMPTY,LID_OPEN` and `EVT,1,DELIVERED`.

### ⚠️ The one rule for CubeIDE code

CubeIDE regenerates code whenever someone edits the `.ioc` file (the pin and peripheral setup screen). **Anything written outside the `/* USER CODE BEGIN */` … `/* USER CODE END */` comments gets deleted.**
- Only write code between those comments.
- Better still, keep the Box logic in its own files, which CubeIDE never touches.
- If you change pins in the `.ioc`, update [docs/pinmap.md](docs/pinmap.md) too.

## 5. Working on the ESP32 (PlatformIO)

### Open the project

1. In VS Code, go to **File → Open Folder…** and choose `parcel-box/esp32`. Open that folder itself, not the repo root, so PlatformIO detects it.
2. Wait for PlatformIO to finish installing the toolchain the first time. It can take a few minutes, and the status bar shows progress.

### Set up your secrets (one-time)

The ESP32 needs the WiFi password, Firebase login and Telegram token. These are **never committed**, because the repo is public.

```bash
cp esp32/include/secrets.example.h esp32/include/secrets.h
```

Then fill in `secrets.h` with the real values. Ask the code owner for them privately. `secrets.h` is gitignored, so it won't be committed. If CI ever says a `secrets.h` was committed, tell the code owner immediately so the keys can be changed.

### Build, upload and monitor

The buttons are in the blue PlatformIO bar at the bottom of VS Code:

| Button | Does | Arduino IDE equivalent |
|---|---|---|
| ✓ | Build | Verify |
| → | Upload to the board | Upload |
| 🔌 | Serial monitor (115200) | Serial Monitor |

If the upload fails with "no serial port" or "failed to connect":
- Use a USB **data** cable; some cables only charge.
- Hold the **BOOT** button, press and release **RST**, then release **BOOT**. Then upload again.
- On Windows, the board may need a **CP210x** or **CH340** USB driver, depending on which USB chip your board has.

### Test the ESP32 without the STM32

You can pretend to be the STM32. Open the serial monitor and type protocol lines, e.g. `STS,OCCUPIED,LID_CLOSED` or `EVT,1,DELIVERED`, and watch the Dashboard and Telegram react. The line format is in [product.md](product.md#uart-protocol-the-contract-between-the-boards).

## 6. Working on the Dashboard (Firebase)

```bash
cd cloud
firebase login            # one-time, opens the browser
firebase deploy           # publishes the Dashboard page + database rules
```

You need to be added to the Firebase project first. Ask the code owner.

## 7. Running the PC tests

The Box logic is tested on your laptop, with no board needed:

```bash
make test
```

To get `gcc` and `make` on Windows, either:
- **WSL** (recommended): run `wsl --install` in PowerShell, restart, then `sudo apt install build-essential` inside Ubuntu; or
- **MSYS2:** install it, then `pacman -S mingw-w64-ucrt-x86_64-gcc make`.

Skipping this is fine too: CI runs the same tests on every pull request.

## 8. How we work

1. **Pick a ticket** from [Issues](https://github.com/aongzera/parcel-box/issues). Only take one whose "blocked by" tickets are all closed.
   - `ready-for-agent`: coding work, normally done with Claude Code.
   - `ready-for-human`: hands-on or setup work. Follow the steps in the ticket.
2. **Make a branch:**
   ```bash
   git checkout -b 5-box-logic-core
   ```
3. **Do the work.** Use the words from [GLOSSARY.md](GLOSSARY.md) in code and commit messages.
4. **Commit and push:**
   ```bash
   git add -A
   git commit -m "Detect Delivery and Collection in the Box logic core"
   git push -u origin 5-box-logic-core
   ```
5. **Open a pull request** on GitHub and write `Closes #5` in the description. Wait for CI to go green ✅, then merge.

Never commit straight to `main`, and never commit `secrets.h`.

### Using AI on this project

- **Claude Code** reads [AGENTS.md](AGENTS.md) automatically. Point it at an issue, e.g. "work on issue #5".
- **Gemini, Antigravity or other chat AIs** need the context pasted in. Give them, in this order:
  1. [product.md](product.md)
  2. [GLOSSARY.md](GLOSSARY.md)
  3. the issue text
  4. the files you're changing

  Tell them to follow the rules in section 11 of product.md.
- Whichever AI you use, **you** must check that the result works (build it, flash it, watch the log) before opening a PR.

## 9. Troubleshooting

| Problem | Try |
|---|---|
| CubeIDE can't find the board | Re-plug the USB cable. Check Device Manager for "ST-Link". Update the ST-LINK firmware when CubeIDE offers. |
| My code vanished after editing the `.ioc` | It was outside `USER CODE` comments. Get it back with `git diff` or `git checkout -- <file>`. |
| Nothing arrives on the ESP32 from the STM32 | Swap the TX/RX wires. Check that both boards share **GND**. Check both are set to 115200 baud. |
| IR sensor never detects the parcel | Turn its potentiometer. Check the EN jumper is on. Try a lighter-coloured parcel. |
| `make: command not found` | See section 7, or just rely on CI. |
| PlatformIO doesn't show its buttons | You opened the repo root. Open the `esp32/` folder instead. |
