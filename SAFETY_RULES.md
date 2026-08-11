# SAFETY RULES — BIPS v3 Firmware
# This file is a MANDATORY checkpoint. Read it before ANY firmware change.
# Devices are ALREADY IN CUSTOMERS' HANDS. There is no physical access to fix.

## Absolute Rules

1. **NEVER BRICK OTA** — Every firmware must boot and be updatable.
   A bricked device = unhappy customer with no recourse.

2. **NEVER BREAK SLEEP** — Every new state MUST have a timeout → idle → sleep.
   No timeout = infinite battery drain = customer returns the product.

3. **NEVER CHANGE OTA URL** — `CONFIG_OTA_URL` must stay at our proxy.
   Changing = devices stop receiving updates permanently.

4. **NEVER ADD EMPTY SECTIONS TO ota.json** — Empty websocket/mqtt/activation
   sections overwrite NVS config = device loses voice capability.

5. **ALWAYS RUN pre_ota_check.py** — Before any deploy, no exceptions.

6. **STATIC ota.json = DIRECT URL** — Never GitHub CDN in fallback.
   ESP32 can't follow 302 redirects.

7. **NEVER RESTART INFRA DURING DOWNLOADS** — Check nginx logs first.

8. **TEST TIMEOUT → IDLE → SLEEP → WAKE** — The full chain, not just timeout.

9. **TOUCH RESTARTS ACTIVATION** — After timeout, touch button must retry.

10. **COMMIT + PUSH + RELEASE BEFORE DEPLOY** — GitHub is the backup.

## Deployment Checklist
[ ] Build succeeds
[ ] pre_ota_check.py passes
[ ] Git commit + push
[ ] GitHub Release with binary
[ ] OTA binary copied to server
[ ] ota.json + ota_proxy_config.json versions match
[ ] bips-ota service restarted
[ ] curl POST returns correct version
[ ] Device test: downloads + reboots
