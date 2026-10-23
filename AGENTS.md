# Smart Parcel Box

Embedded System Lab final project: an STM32 + ESP32-S3 parcel box that detects Deliveries and Collections and alerts the Owner. Read `product.md` (the project overview), `GLOSSARY.md` and `docs/adr/` before working.

## CI

`.github/workflows/ci.yml` runs on every push to `main` and on every PR. It has three jobs:
- `make test` from the repo root runs the Box logic core tests on a PC. Keep that command as the entry point.
- The ESP32 project in `esp32/` must build, using `secrets.example.h` as placeholder secrets.
- The build fails if any `secrets.h` is committed.

Jobs whose project doesn't exist yet skip themselves.

## Agent skills

### Issue tracker

Issues and specs live in GitHub Issues on aongzera/parcel-box, managed with the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

The five default labels: needs-triage, needs-info, ready-for-agent, ready-for-human, wontfix. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
