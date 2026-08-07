# BIPS v3 — FINAL PLAN for shipping

## Changes to make (in order):

### Fix 1: Display orientation (CRITICAL)
- File: main/boards/bips-v3/config.h
- Change: MIRROR_X=true, MIRROR_Y=true
- Reason: LVGL callback overrides SH1106 init. With fixed driver,
  mirror(true,true) sends 0xA1+0xC8 = 180° rotation = correct for this display.

### Fix 2: Display init revert
- File: managed_components/tny-robotics__sh1106-esp-idf/esp_lcd_panel_sh1106.c
- Change: Revert init back to 0xA1+0xC8 (original)
- Reason: LVGL overrides it anyway, but keep it consistent. If LVGL callback
  ever doesn't run, we still get correct 180°.

### Fix 3: Power save timer (CRITICAL)
- File: main/boards/bips-v3/bips_v3_board.cc
- Change: Remove ResetActivity() from SetPowerSaveLevel()
- Reason: Audio service calls SetPowerSaveLevel() every second,
  resetting idle_seconds_ to 0. Timer never reaches 60.

### Fix 4: Version sync
- File: CMakeLists.txt
- Change: PROJECT_VER = "4.0.4"
- File: /home/openclaw/bips-ota/ota.json
- Change: version = "4.0.4" (already done)

### Fix 5: NTP robustness (nice to have)
- Already implemented, should work after WiFi connects.
- pool.ntp.org may have DNS delay on first boot.

## Files NOT to change:
- bips_v3_board.cc — Tavily code is correct, don't touch
- WiFi config HTML — Tavily field is added correctly
- WiFi config backend — NVS read/write for tavily_api_key works

## Testing after OTA:
1. Reboot device → should download v4.0.4
2. Display: check orientation (should be correct now)
3. Wait 60s: display should turn off
4. Touch button: display should turn back on
5. Wait 5 min: device should enter deep sleep
6. Time: should sync via NTP within 30s of WiFi connect

## Emergency rollback:
- Restore from backups/v3.9.7-working/ if anything breaks
- Set OTA version to 3.9.7 to stop update loop
