/*
 * Placeholder secrets for the ESP32. Copy this file to secrets.h (gitignored)
 * in the same folder and fill in the real values, which you get privately
 * from the code owner. Never commit secrets.h, and never paste these values
 * into issues, PRs or the group chat (product.md section 5, "Secrets").
 * CI builds with this file copied as is.
 */
#ifndef SECRETS_H
#define SECRETS_H

/* The WiFi network the Box joins. */
#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

/* Firebase (docs/guides/firebase.md, steps 2 to 4). */
#define FIREBASE_API_KEY      "your-firebase-api-key"
#define FIREBASE_DATABASE_URL "https://your-project-default-rtdb.asia-southeast1.firebasedatabase.app"
/* The Box's device account, the only login allowed to write. */
#define FIREBASE_USER_EMAIL    "box@parcel-box.local"
#define FIREBASE_USER_PASSWORD "your-device-account-password"

/* Telegram (docs/guides/telegram.md). */
#define TELEGRAM_BOT_TOKEN "123456789:your-bot-token"
#define TELEGRAM_CHAT_ID   "-1001234567890"

#endif /* SECRETS_H */
