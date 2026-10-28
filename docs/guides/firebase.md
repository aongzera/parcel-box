# Firebase: the Box's memory in the cloud

> **Who this is for:** anyone on the team, no embedded knowledge needed. If you can use a website and copy-paste into a terminal, you can do this.
> **Related tickets:** [#3 Set up the Firebase project](https://github.com/aongzera/parcel-box/issues/3) · [#6](https://github.com/aongzera/parcel-box/issues/6) · [#8](https://github.com/aongzera/parcel-box/issues/8)

---

## 1. What is Firebase?

Firebase is a free set of Google cloud services for apps. We only use two of them:

| Firebase service | What it is | Everyday comparison |
|---|---|---|
| **Realtime Database** | An online database that pushes changes to everyone watching it, instantly | A shared Google Sheet: when one person types, everyone sees it change without refreshing |
| **Hosting** | Free web hosting for our Dashboard page | A free web address where our page lives |

We also turn on **Authentication** (logins), so that only our Box is allowed to write data.

## 2. What does it do in our project?

```
Sensors → STM32 → ESP32 ──writes──▶ [ FIREBASE ] ──live──▶ Dashboard
                        └──────────▶ Telegram
```

The ESP32 (the WiFi chip in the Box) is the **only thing that writes** to Firebase. The Dashboard **only reads** from it. Firebase sits in the middle and stores two things:

| What | Example | Used for |
|---|---|---|
| **Box status**: what the Box looks like right now | Occupied, Lid closed, last seen 14:32:05 | The big badges and the online/offline light on the Dashboard |
| **Event log**: a history of everything that happened | 14:32 DELIVERED, 18:05 COLLECTED | The history table on the Dashboard |

## 3. Why do we need it?

- **The course requires a cloud system.** Firebase is our cloud.
- **The ESP32 and the Dashboard never talk directly.** The Box is on someone's WiFi and the Dashboard is on someone's phone. They can't easily find each other, but both can reach Firebase. It's the meeting point.
- **It keeps history.** The ESP32 has a tiny memory that gets wiped when it loses power. Firebase keeps the Event log for good.
- **It's live.** The Dashboard updates the moment the Box changes, without refreshing and without us writing a server.

**Why not NETPIE**, which the labs used? NETPIE is fine for live values, but awkward for keeping a list of past events. The full reasoning is in [ADR 0002](../adr/0002-firebase-over-netpie.md).

---

## 4. Overview: the 6 steps

| # | Step | Where you do it | Time | Who |
|---|---|---|---|---|
| 1 | Create the Firebase project | Website (Firebase console) | 5 min | Anyone (ticket #3) |
| 2 | Create the Realtime Database | Website | 3 min | Anyone (ticket #3) |
| 3 | Create the Box's login (device account) | Website | 5 min | Anyone (ticket #3) |
| 4 | Register a web app and copy the config | Website | 3 min | Anyone (ticket #3) |
| 5 | Lock down who can write (security rules) | Repo + terminal | 15 min | Code owner (ticket #6) |
| 6 | Test that data shows up | Website | 5 min | Anyone |

Steps 1–4 are just clicking through a website. Steps 5–6 happen once the code exists.

---

## 5. Step by step

### Step 1: Create the Firebase project

1. Go to **https://console.firebase.google.com** and sign in with a Google account. Use one the team can keep access to, not a throwaway.
2. Click **Create a project** (or **Add project**).
3. Name it `smart-parcel-box`. Firebase may add random letters to the ID, e.g. `smart-parcel-box-3f2a1`. Write the ID down, because you'll need it later.
4. When it asks about **Google Analytics**, switch it **off**. We don't need it.
5. Click **Create project** and wait about 30 seconds.

✅ **Done when:** you see the project's home page with its name at the top.

> 💸 **Cost:** everything here fits in the free **Spark** plan. If Firebase ever asks for a credit card or the "Blaze" plan, stop and ask the code owner. We shouldn't need it.

### Step 2: Create the Realtime Database

1. In the left menu: **Build → Realtime Database → Create Database**.
2. **Location:** choose **Singapore (asia-southeast1)**, the closest to Thailand, which means faster updates.
3. **Security rules:** choose **Start in locked mode**. Nobody can read or write until we set rules in step 5. That's the safe default.
4. Click **Enable**.
5. At the top of the page you'll see the database URL, like `https://smart-parcel-box-3f2a1-default-rtdb.asia-southeast1.firebasedatabase.app`. **Copy it**; it's part of the config.

✅ **Done when:** you see an empty database page showing that URL and `null`.

### Step 3: Create the Box's login (device account)

Our rules will say "only the Box may write". For that, the Box needs its own login, just like a person has an email and password.

1. Left menu: **Build → Authentication → Get started**.
2. Under **Sign-in method**, click **Email/Password**, switch the first toggle **on**, and click **Save**.
3. Go to the **Users** tab → **Add user**:
   - Email: `box@parcel-box.local` (it doesn't need to be a real inbox)
   - Password: a long random password, e.g. from a password generator
4. Click **Add user**. In the list, copy the user's **User UID**, a long code like `Xy12Ab…`.

✅ **Done when:** one user appears in the Users list, and you have saved its **email, password and UID**.

### Step 4: Register a web app and copy the config

The Dashboard and the ESP32 both need a few settings to find our project.

1. Click the ⚙️ gear next to **Project Overview** → **Project settings**.
2. Scroll to **Your apps** → click the **`</>`** (Web) icon.
3. Nickname: `dashboard`. Tick **"Also set up Firebase Hosting"**, then click **Register app**.
4. Firebase shows a block of code with `const firebaseConfig = { … }`. Copy these values:
   - `apiKey`
   - `authDomain`
   - `databaseURL` (if it's missing, use the URL from step 2)
   - `projectId`
   - `appId`
5. Click through the remaining screens; we'll install the tools later.
6. Back in **Project settings → Users and permissions**, click **Add member**, add the code owner's Google email, and give them the **Owner** role.

**Send the code owner privately (a DM, not the group chat):**
- the config values from step 4;
- the device account's email, password and UID from step 3.

✅ **Done when:** the code owner confirms they received everything and can open the project.

> 🔐 **What's actually secret?**
> - The **device password is a real secret.** Anyone who has it can write fake Events. It goes only into the ESP32's `secrets.h`, which is never committed.
> - The **web config (`apiKey` etc.) is not really secret.** It ends up inside the Dashboard page, which anyone can view. Firebase is designed for this: our protection is the **security rules** in step 5, not hiding the apiKey.

### Step 5: Lock down who can write (security rules)

The rules are a small file in the repo that tells Firebase who may read and write. Ours says: **anyone can read the Box, only the Box's login can write.**

1. Install the Firebase command-line tool (needs [Node.js LTS](https://nodejs.org)):
   ```bash
   npm install -g firebase-tools
   ```
2. Log in (this opens your browser):
   ```bash
   firebase login
   ```
3. From the repo root, set up the `cloud/` folder (skip this if `cloud/firebase.json` already exists):
   ```bash
   cd cloud
   firebase init
   ```
   Answer the questions:
   - Features: select **Realtime Database** and **Hosting** (space to tick, enter to continue).
   - Project: **Use an existing project** → `smart-parcel-box-…`
   - Database rules file: `database.rules.json`
   - Hosting public directory: `public`
   - Single-page app: **No**. Automatic GitHub builds: **No**.
4. Put this in `cloud/database.rules.json`, replacing `DEVICE_UID` with the UID from step 3:
   ```json
   {
     "rules": {
       "box": {
         ".read": true,
         ".write": "auth != null && auth.uid === 'DEVICE_UID'"
       }
     }
   }
   ```
   In plain words: everything under `box` can be read by anyone, but written only by someone logged in as the Box. Nothing outside `box` can be read or written at all. (The UID isn't a secret, so it's fine to commit.)
5. Upload the rules:
   ```bash
   firebase deploy --only database
   ```

✅ **Done when:** the console's **Realtime Database → Rules** tab shows the same rules.

### Step 6: Test that data shows up

Until the ESP32 code exists, you can fake the Box by hand.

1. Console → **Realtime Database → Data** tab.
2. Hover over the root and click **+**. Add:
   - key `box` → then under it, key `status` → then under that, key `state` with value `OCCUPIED`.
3. You'll see the tree update. The console ignores the rules because you're the project owner, so this always works.
4. Delete it again with the 🗑️ icon when you're done.

Once ticket #6 is done, the real test is to type `STS,OCCUPIED,LID_CLOSED` into the ESP32's serial monitor and watch `box/status` change in the console by itself.

✅ **Done when:** you've seen data appear in the console, either typed by you or sent by the ESP32.

---

## 6. How our data is laid out

You don't need to memorise this, but it helps when you're looking at the console:

```
box/
├── status/
│   ├── state:          "EMPTY" | "OCCUPIED"
│   ├── lid:            "OPEN" | "CLOSED"
│   ├── lastDeliveryAt: when the last Delivery happened
│   └── lastSeenAt:     the last time the ESP32 checked in (for online/offline)
└── events/
    ├── <auto id>: { type: "DELIVERED", at: <time or "unknown">, seq: 7 }
    └── <auto id>: { type: "COLLECTED", at: …, seq: 8 }
```

## 7. When something goes wrong

| Problem | Likely cause |
|---|---|
| "Permission denied" when the ESP32 writes | The rules' UID doesn't match the device account's UID, or the ESP32 has the wrong password. |
| The Dashboard shows nothing | The rules aren't deployed (still in locked mode), or the Dashboard has the wrong `databaseURL`. |
| `firebase: command not found` | Node.js isn't installed, or close and reopen the terminal after `npm install -g firebase-tools`. |
| Asked to upgrade to Blaze | Stop and ask. Something is set up beyond what we need. |
