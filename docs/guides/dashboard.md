# Dashboard: the Owner's window into the Box

> **Who this is for:** anyone on the team. If you've ever edited an HTML file, you're overqualified. No embedded knowledge needed.
> **Related tickets:** [#6 Live Box status](https://github.com/aongzera/parcel-box/issues/6) · [#8 Event log](https://github.com/aongzera/parcel-box/issues/8)
> **Read first:** [firebase.md](firebase.md). The Dashboard reads everything from Firebase.

---

## 1. What is the Dashboard?

A **single web page**, hosted for free on Firebase Hosting, that shows what the Box is doing **right now** and **what happened before**. Anyone with the link can open it on a phone or laptop: no app, no login.

It looks roughly like this:

```
┌──────────────────────────────────────────────┐
│  Smart Parcel Box                  ● online  │
│                                              │
│  [ OCCUPIED ]   [ LID CLOSED ]               │
│  Last Delivery: today 14:32                  │
│                                              │
│  Event log                                   │
│  14:32  DELIVERED                            │
│  12:10  OPENED_EMPTY                         │
│  09:05  COLLECTED                            │
└──────────────────────────────────────────────┘
```

## 2. What does it do in our project?

```
Sensors → STM32 → ESP32 → Firebase ──live──▶ [ DASHBOARD ]
```

It's the **end of the line**. It never talks to the Box directly. It just watches Firebase and redraws whenever Firebase changes. It shows **exactly four things**, and nothing else:

| # | What it shows | Where the data comes from |
|---|---|---|
| 1 | **Empty / Occupied** and **Lid open / closed** badges | `box/status/state` and `box/status/lid` |
| 2 | **Time of the last Delivery** | `box/status/lastDeliveryAt` |
| 3 | **Event log** table, newest first | `box/events` |
| 4 | **Online / offline** light: offline if the Box hasn't checked in for ~30 s | `box/status/lastSeenAt` |

It is **read-only**: no buttons that change the Box, no login. That keeps it simple and safe.

## 3. Why do we need it?

- **It's what the Owner (and the graders) actually see.** In the demo, the Dashboard flipping from Empty to Occupied the moment a parcel drops in is the "wow" moment.
- **Telegram tells you something happened. The Dashboard tells you the current state and the history.** "Is there a parcel in my Box right now?" is answered here.
- **It proves the cloud part works.** The course requires a cloud connection, and this is the visible proof.
- **It shows whether the Box is alive.** If the Box loses power or WiFi, the light goes offline, so silence isn't mistaken for "nothing happened".

---

## 4. Overview: the 6 steps

| # | Step | Time | Needs |
|---|---|---|---|
| 1 | Install the tools and get the project files | 15 min | Node.js, Firebase steps 1–5 done |
| 2 | Connect the page to Firebase | 20 min | Firebase web config |
| 3 | Show the live Box status (badges + online light) | 1 h | Step 2 |
| 4 | Show the Event log and last Delivery | 1 h | Step 3 |
| 5 | Test on your laptop with fake data | 20 min | Steps 3–4 |
| 6 | Publish it online and check on a phone | 10 min | Step 5 |

Steps 3–4 are the real coding. In our team that's done with Claude Code (tickets #6 and #8). Everyone should still understand steps 1, 5 and 6, so you can run, test and show the Dashboard yourself.

---

## 5. Step by step

### Step 1: Install the tools and get the project files

1. Install **[Node.js LTS](https://nodejs.org)**. It's needed for the Firebase command-line tool.
2. Install the Firebase tool and log in:
   ```bash
   npm install -g firebase-tools
   firebase login
   ```
3. Get the latest code (see the [README](../../README.md) if you haven't cloned it yet):
   ```bash
   git pull
   ```
4. The Dashboard lives in `cloud/`:
   ```
   cloud/
   ├── firebase.json          ← tells Firebase what to upload
   ├── database.rules.json    ← who may read/write (see firebase.md)
   └── public/
       └── index.html         ← THE Dashboard page
   ```
   If `cloud/` doesn't exist yet, it gets created in [firebase.md step 5](firebase.md#step-5-lock-down-who-can-write-security-rules).
5. Ask the code owner to add you to the Firebase project, so you can deploy.

✅ **Done when:** `firebase projects:list` shows `smart-parcel-box-…`.

### Step 2: Connect the page to Firebase

The page loads Google's Firebase library straight from the internet, so there's nothing to install. It needs the **web config** from [firebase.md step 4](firebase.md#step-4-register-a-web-app-and-copy-the-config). This is the core of how it works:

```html
<script type="module">
  import { initializeApp } from "https://www.gstatic.com/firebasejs/10.12.2/firebase-app.js";
  import { getDatabase, ref, onValue } from "https://www.gstatic.com/firebasejs/10.12.2/firebase-database.js";

  const app = initializeApp({
    apiKey: "…", authDomain: "…", databaseURL: "…", projectId: "…", appId: "…"
  });
  const db = getDatabase(app);

  // "Call me now, and every time box/status changes."
  onValue(ref(db, "box/status"), (snapshot) => {
    const status = snapshot.val();   // e.g. { state: "OCCUPIED", lid: "CLOSED", … }
    console.log(status);
  });
</script>
```

**The one idea to understand:** `onValue` is a **subscription**. You don't ask "what's the status?" over and over. You say "tell me whenever it changes", and Firebase pushes every update to the page instantly. That's why the Dashboard never needs refreshing.

> It's fine to commit the web config. It's visible to anyone who opens the page anyway, and our security comes from the database rules. **Never** put the ESP32's device password here.

✅ **Done when:** you open the page, press F12 → Console, and see the status object printed (or `null` if the database is empty).

### Step 3: Show the live Box status

Turn that `status` object into what people see:

| If… | Show |
|---|---|
| `state === "OCCUPIED"` | Big badge "OCCUPIED" (strong colour) |
| `state === "EMPTY"` | Big badge "EMPTY" (calm colour) |
| `lid === "OPEN"` / `"CLOSED"` | Second badge "LID OPEN" / "LID CLOSED" |
| `lastSeenAt` is less than ~30 s ago | ● online (green) |
| `lastSeenAt` is older, or missing | ○ offline (grey), plus "last seen 14:05" |

The online check must **re-run every few seconds** (e.g. with `setInterval`), not only when data changes. If the Box dies, no more data arrives, so nothing else would trigger the switch to offline.

✅ **Done when:** changing `box/status/state` in the Firebase console flips the badge on the page within a second, with no refresh.

### Step 4: Show the Event log and last Delivery

1. Subscribe to `box/events` the same way, with `onValue(ref(db, "box/events"), …)`.
2. Turn the events into a list, **sort newest first** by `at`, and draw a table: time | event.
3. Show times in Thai local time, e.g. `14:32` or `Mon 5 Nov 14:32`.
4. If `at` is `"unknown"` (the Box hadn't synced its clock yet), show **"time unknown"**. **Never show 1970.** That's what you get if you treat "no time" as zero.
5. Show `lastDeliveryAt` as "Last Delivery: …".
6. Use friendly names in the table: `DELIVERED` → "Parcel delivered", `COLLECTED` → "Parcels collected", `OPENED_OCCUPIED` → "Opened, parcel still inside", `OPENED_EMPTY` → "Opened, nothing inside".

✅ **Done when:** adding an event in the console makes a new row appear at the top of the table.

### Step 5: Test on your laptop with fake data

1. Start a local copy of the site:
   ```bash
   cd cloud
   firebase emulators:start --only hosting
   ```
   Then open **http://localhost:5000**. If it says the port is busy, use the address it prints.
2. In the Firebase console (**Realtime Database → Data**), add or change data by hand and watch the page react:

   | Try this | Expect |
   |---|---|
   | Set `box/status/state` to `OCCUPIED` | Badge turns OCCUPIED |
   | Set `box/status/lid` to `OPEN` | Lid badge says OPEN |
   | Add an event `{ type: "DELIVERED", at: <now>, seq: 1 }` | New row at the top |
   | Add an event with `at: "unknown"` | Row says "time unknown" |
   | Wait 30+ s without updating `lastSeenAt` | Light goes offline |

3. Once the ESP32 is working (ticket #6), test it for real: type `STS,OCCUPIED,LID_CLOSED` into its serial monitor and the page should change by itself.

✅ **Done when:** every row in the table above behaves as expected.

### Step 6: Publish it online and check on a phone

```bash
cd cloud
firebase deploy --only hosting
```

It prints a **Hosting URL** like `https://smart-parcel-box-3f2a1.web.app`. That's the public Dashboard link. Open it on your phone over mobile data (not your WiFi), to prove it works from anywhere.

✅ **Done when:** the page loads on a phone, shows live data, and the link is posted in the team group.

---

## 6. When something goes wrong

| Problem | Likely cause |
|---|---|
| The page is blank, with a red error in the F12 console | A typo in the web config, or the `import` URLs are wrong. |
| "Permission denied" in the console | The database rules aren't deployed yet; see [firebase.md step 5](firebase.md#step-5-lock-down-who-can-write-security-rules). |
| The page shows old data | You're looking at an old deploy. Run `firebase deploy --only hosting` again, then hard-refresh with Ctrl+Shift+R. |
| Always "offline" | The ESP32 isn't updating `lastSeenAt`, or your laptop's clock is wrong. |
| The table shows 1970 | An event with an unknown time is being treated as 0. Handle `"unknown"` (step 4.4). |
