# BIPS v3 — Status & Next Steps (2026-08-07 02:30)

## Текущая версия: v4.0.3 (на OTA)

## Что работает ✅
- Голосовой ассистент через xiaozhi.me
- Touch кнопка (GPIO43, TTP223, active_high=true, ToggleChatState)
- Boot кнопка (GPIO0, WiFi config / toggle chat)
- Двойной клик BOOT = WiFi reset
- Динамик, микрофон, WiFi
- NTP синхронизация времени (UTC+3 Москва)
- Tavily web search MCP tool (self.web_search)
- LED (GPIO48)

## Что НЕ работает ❌
- **Дисплей перевёрнут** — SH1106 driver init ставит 180° (0xA1+0xC8).
  mirror(false,false) отправляет 0xA0+0xC0, но LVGL port вызывает mirror()
  ДО создания OledDisplay, а потом LVGL может переопределить.
  Нужно проверить: MIRROR_X=false, MIRROR_Y=false — тестировать на устройстве.
- **Экран не гаснет** — Activity timer ресетится из-за SetPowerSaveLevel().
  Нужно убрать ResetActivity() из SetPowerSaveLevel() — только кнопки должны ресетить.
- **Tavily не протестирован** — нужен API ключ через WiFi config (Advanced tab)

## Ключевые файлы
- Board: ~/projects/bips-v3-firmware/main/boards/bips-v3/bips_v3_board.cc
- Config: ~/projects/bips-v3-firmware/main/boards/bips-v3/config.h
- SDK: ~/projects/bips-v3-firmware/sdkconfig
- SH1106 driver: managed_components/tny-robotics__sh1106-esp-idf/esp_lcd_panel_sh1106.c
  - mirror_x ИСПРАВЛЕН (был 0xA6/0xA7 вместо 0xA0/0xA1)
  - init ставит 0xA1+0xC8 по умолчанию
- WiFi config HTML: managed_components/78__esp-wifi-connect/assets/wifi_configuration.html
  - Tavily API key поле добавлено в Advanced tab
- WiFi config backend: managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc
  - NVS read/write для tavily_api_key добавлен
- OTA server: /home/openclaw/bips-ota/ (server.py + ota.json + xiaozhi.bin)
- OTA systemd: ~/.config/systemd/user/bips-ota-server.service

## Бэкапы
- backups/v3.9.7-working/ — стабильная база (touch fix, без display fix)
- backups/v3.9.8-FINAL/ — display fix (крашится при OTA)
- backups/v3.9.6-working-display/ — raw I2C display fix

## Git commits (последние)
- v4.0.3 attempt 2: MIRROR_X=false MIRROR_Y=false
- Fix: PROJECT_VER must match OTA version for updates to work
- v4.0.3: Fix SH1106 driver mirror bug + MIRROR_X=true
- v4.0.2: NTP time sync + display mirror fix
- v4.0.1: display mirror fix (180° rotation via raw I2C)
- v4.0.0: Tavily web search + activity-based power save

## Следующие шаги (приоритет)
1. **Display orientation** — тестировать MIRROR_X=false, MIRROR_Y=false на устройстве
   - Если не поможет: попробовать MIRROR_X=true, MIRROR_Y=false
   - Если не поможет: попробовать MIRROR_X=false, MIRROR_Y=true
   - Если не поможет: нужно разобраться с LVGL port (esp_lvgl_port_disp.c)
2. **Power save** — убрать ResetActivity() из SetPowerSaveLevel(), только кнопки
3. **Tavily** — протестировать с реальным API ключом
4. **Динамик бубнит внутрь** — отверстия слишком маленькие, нужен звуковой туннель или щель

## Важно
- **НЕ включать blank_nvs.bin** в flash пакет!
- **PROJECT_VER** должен совпадать с версией в ota.json!
- Raw I2C ПОСЛЕ OledDisplay крашит OTA. ПЕРЕД — безопасно.
- SH1106 mirror_x баг: использует 0xA6/0xA7 (INVERT) вместо 0xA0/0xA1 (SEGMENT REMAP) — ИСПРАВЛЕН в драйвере.
