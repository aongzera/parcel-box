# Firebase, not NETPIE, for the cloud

The course labs use NETPIE, but we use Firebase Realtime Database plus Firebase Hosting with a custom Dashboard. The Event log is a list of timestamped Events, which Firebase stores natively and a web page can watch live with very little code. NETPIE's Freeboard is fine for live values but awkward for history. We write the code mostly with AI, so building a custom page costs hours, not days. Alerts go to the Owner through a Telegram bot that the ESP32 calls directly. That part does not depend on the choice of cloud.

Secrets (WiFi credentials, the bot token, Firebase keys) live in a gitignored `esp32/include/secrets.h`, with a committed `secrets.example.h` showing the format, because the repo must be public.
