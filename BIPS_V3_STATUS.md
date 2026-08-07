# BIPS v3 Firmware Status

## Current Version: v4.0.4

**Last updated:** 2026-08-07

## What Works

- ✅ Display (SH1106 1.3" OLED, 180° rotation)
- ✅ Touch button (GPIO43, TTP223, active_high)
- ✅ Boot button (GPIO0)
- ✅ Speaker (I2S, 16kHz)
- ✅ Microphone (I2S)
- ✅ LED (GPIO48)
- ✅ WiFi connectivity
- ✅ OTA updates (GitHub-based)
- ✅ NTP time sync (pool.ntp.org, UTC+3 Moscow)
- ✅ Tavily web search (needs API key)
- ✅ Activity-based power save (60s display off, 300s deep sleep)
- ✅ Russian language

## Known Issues

- ⚠️ Display orientation needs testing (v4.0.4 changes)
- ⚠️ Power save timer needs testing (removed ResetActivity from SetPowerSaveLevel)
- ⚠️ NTP sync delay on cold boot (5-30 seconds)
- ⚠️ Tavily API key not configured (user needs to set via WiFi config)

## Hardware

- **Board:** XH-S3E-AI V1.0
- **MCU:** ESP32-S3 N16R8 (16MB flash, 8MB PSRAM)
- **Display:** SH1106 1.3" OLED (I2C, 128x64)
- **Audio:** 16kHz output, 16kHz input
- **Buttons:** Boot (GPIO0), Touch (GPIO43)
- **LED:** GPIO48
- **Pins:** SDA=GPIO41, SCL=GPIO42, TOUCH=GPIO43

## OTA Configuration

- **URL:** `https://raw.githubusercontent.com/USER/bips-firmware/bips-v3/ota.json`
- **Version:** 4.0.4
- **Binary:** `https://github.com/USER/bips-firmware/releases/download/bips-v3/v4.0.4/xiaozhi.bin`

## Build

```bash
cd ~/projects/bips-v3-firmware
source ~/esp-idf/export.sh
idf.py build
```

## Flash (USB)

```bash
esptool.py --chip esp32s3 -b 460800 write-flash \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0xd000 build/ota_data_initial.bin \
    0x20000 build/xiaozhi.bin \
    0x800000 build/generated_assets.bin
```

## Deploy to OTA

```bash
# 1. Build
idf.py build

# 2. Create GitHub release with xiaozhi.bin as asset

# 3. Update ota.json with new version and URL

# 4. Commit and push
git add ota.json
git commit -m "OTA: v4.0.5"
git push origin bips-v3
```

## Recent Changes

### v4.0.4 (2026-08-07)
- Fixed display orientation (MIRROR_X=true, MIRROR_Y=true)
- Reverted SH1106 init to original 0xA1+0xC8
- Fixed power save timer (removed ResetActivity from SetPowerSaveLevel)
- Synced PROJECT_VER with OTA version
- Added NTP sync via pool.ntp.org

### v4.0.3 (2026-08-07)
- Added Tavily web search MCP tool
- Added activity-based power save (60s display off, 300s deep sleep)
- Added NTP time sync (UTC+3 Moscow)

### v3.9.7 (2026-08-07)
- Fixed touch button (active_high=true for TTP223)
- Stable baseline version

## Next Steps

1. Test v4.0.4 display orientation
2. Test power save timer
3. Configure Tavily API key
4. Deploy to customer
