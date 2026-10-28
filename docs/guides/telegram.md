# Telegram: the Box taps the Owner on the shoulder

> **Who this is for:** anyone on the team. If you can use Telegram and open a link in a browser, you can do steps 1–4.
> **Related tickets:** [#4 Create the Telegram bot and team group](https://github.com/aongzera/parcel-box/issues/4) · [#9 Telegram Alerts](https://github.com/aongzera/parcel-box/issues/9)

---

## 1. What is a Telegram bot?

Telegram is a chat app, like LINE. A **bot** is a Telegram account that is run by code instead of a person. Telegram gives every bot:
- a **token**, a long password like `7012345678:AAH…`;
- a simple web address our code can call to say "send this message to that chat".

So "sending an Alert" is just the ESP32 visiting a special Telegram URL with the message in it. It's the same idea as typing a URL into a browser.

## 2. What does it do in our project?

```
Sensors → STM32 → ESP32 → Firebase → Dashboard
                     └──▶ [ TELEGRAM ] ──▶ 📱 Owner's phone buzzes
```

Whenever the Box decides something happened (an **Event**), the ESP32 sends a short message to our team's Telegram group. That message is called an **Alert**.

| Event | Alert? | Message |
|---|---|---|
| Delivery | ✅ | 📦 Parcel delivered at 14:32 |
| Collection | ✅ | 📭 Parcels collected at 18:05 |
| Opened while occupied | ✅ | ⚠️ Box opened, a parcel is still inside (09:12) |
| Opened while empty | ❌ | Nothing; it only goes in the Dashboard's Event log |

"Opened while empty" doesn't alert because it's harmless (someone peeked), and alerting on it would just be noise.

For the demo, the **team group plays the Owner**: everyone in it, plus the demo phone, sees the Alerts live.

## 3. Why do we need it?

- **The Dashboard only helps if you're looking at it.** Telegram comes to you: the phone buzzes the moment a parcel lands.
- **That's the whole point of the product.** "Detect a delivery and **alert the owner**" is the project's one-line description.
- **LINE Notify was shut down in 2025.** Telegram's bot system is free, simple, and needs no business account. LINE's replacement (Messaging API) takes much more setup.
- **It's great for the demo.** A phone buzzing on the table is very convincing.

---

## 4. Overview: the 6 steps

| # | Step | Where | Time | Who |
|---|---|---|---|---|
| 1 | Create the bot and get its token | Telegram app | 5 min | Anyone (ticket #4) |
| 2 | Create the team group and add the bot | Telegram app | 5 min | Anyone (ticket #4) |
| 3 | Find the group's chat ID | Browser | 5 min | Anyone (ticket #4) |
| 4 | Send a test message from the browser | Browser | 2 min | Anyone (ticket #4) |
| 5 | Hand the token and chat ID to the code owner, privately | DM | 1 min | Whoever did 1–4 |
| 6 | The ESP32 sends real Alerts | Code | ~1 h | Code owner (ticket #9) |

Steps 1–5 need **no coding at all**.

---

## 5. Step by step

### Step 1: Create the bot and get its token

1. In Telegram, search for **@BotFather** (the official one, with a blue tick) and open the chat.
2. Send `/newbot`.
3. It asks for a **name**, which people see. Send: `Smart Parcel Box`.
4. It asks for a **username**, which must end in `bot` and be unique. Try e.g. `smart_parcel_box_cu_bot`; if it's taken, add numbers.
5. BotFather replies with a **token** like `7012345678:AAHk9…`. Copy it somewhere private.

> 🔐 **The token is a password.** Anyone with it can send messages as our bot. Never post it in the group, on GitHub, or in a screenshot. If it leaks, send `/revoke` to BotFather to get a new one.

✅ **Done when:** you have the token saved privately.

### Step 2: Create the team group and add the bot

1. In Telegram: **New Group** → add all three team members → name it e.g. `Parcel Box Alerts`.
2. Open the group → tap its name → **Add members** → search your bot's username → add it.
3. In the group, send this message. It must start with `/` so the bot can see it:
   ```
   /hello@your_bot_username
   ```
   Why the `/`? By default a bot in a group ignores normal chat (Telegram's "privacy mode") and only sees commands. This one message is just so step 3 can find the group.

✅ **Done when:** the bot appears in the group's member list.

### Step 3: Find the group's chat ID

The bot needs to know **which chat** to post into. Every chat has an ID number.

1. In a browser, open this address, replacing `<TOKEN>` with your token:
   ```
   https://api.telegram.org/bot<TOKEN>/getUpdates
   ```
2. You'll see a page of text (JSON). Look for `"chat":{"id":` followed by a **negative number**, e.g. `-1001234567890` or `-4567891234`. Check that the `"title"` next to it is your group's name.
3. Copy the whole number, **including the minus sign**.

If the page shows `"result":[]` (empty), send the `/hello@your_bot_username` message in the group again, then reload the page.

✅ **Done when:** you have a negative chat ID that belongs to your group.

### Step 4: Send a test message from the browser

This proves the token and chat ID work, before any code is involved. Open this address, replacing both parts:

```
https://api.telegram.org/bot<TOKEN>/sendMessage?chat_id=<CHAT_ID>&text=Hello%20from%20the%20Box
```

The bot should post **"Hello from the Box"** in the group, and the browser shows `"ok":true`.

| Browser shows | Meaning |
|---|---|
| `"ok":true` | 🎉 Everything works |
| `401 Unauthorized` | The token is wrong. Copy it again from BotFather. |
| `400 chat not found` | The chat ID is wrong (check the minus sign), or the bot isn't in the group. |

✅ **Done when:** the test message appears in the group.

### Step 5: Hand the token and chat ID to the code owner, privately

Send both values to the code owner in a **private message**. They go into the ESP32's `secrets.h` file, which is never uploaded to GitHub:

```c
#define TELEGRAM_BOT_TOKEN "7012345678:AAH…"
#define TELEGRAM_CHAT_ID   "-1001234567890"
```

Then tick ticket #4 as done on the tracker, or close it on GitHub.

✅ **Done when:** the code owner confirms they have both.

### Step 6: The ESP32 sends real Alerts (ticket #9)

This part is code, built with Claude Code. Here's what happens, so everyone can explain it at the demo:

1. The STM32 decides an Event happened and sends `EVT,7,DELIVERED` to the ESP32 over the wire between them.
2. The ESP32 saves it to Firebase, then checks the type. It's **not** OPENED_EMPTY, so it alerts.
3. The ESP32 turns it into friendly text: "📦 Parcel delivered at 14:32".
4. The ESP32 calls the same `sendMessage` address you used in step 4, over secure HTTPS.
5. If Telegram can't be reached (no internet), the ESP32 **doesn't freeze**. Saving to Firebase still happens.

**How to test it** without the STM32: plug in the ESP32, open its serial monitor, and type:

| Type | Expect in the group |
|---|---|
| `EVT,1,DELIVERED` | 📦 Parcel delivered at … |
| `EVT,2,COLLECTED` | 📭 Parcels collected at … |
| `EVT,3,OPENED_OCCUPIED` | ⚠️ Box opened, a parcel is still inside |
| `EVT,4,OPENED_EMPTY` | *Nothing* (correct!) |

✅ **Done when:** all four rows behave as expected.

---

## 6. When something goes wrong

| Problem | Likely cause |
|---|---|
| `getUpdates` is empty | The bot never saw a message. Send `/hello@your_bot_username` in the group, then reload. |
| The chat ID suddenly stops working | The group was upgraded to a "supergroup" (Telegram does this automatically), which changes its ID to one starting with `-100`. Repeat step 3. |
| The test message works in the browser but not from the ESP32 | Check `secrets.h` for typos, and that the ESP32 is actually on WiFi (look at its serial monitor). |
| The token got posted somewhere public | `/revoke` in BotFather, then send the new token to the code owner privately. |
