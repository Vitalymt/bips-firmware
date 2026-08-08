# BIPS v3 Firmware — Safety Rules

**Этот документ — ЗАКОН для всех, кто работает с прошивкой БИПС.
Нарушение = бракованные устройства у клиентов = возврат денег = потеря проекта.**

---

## 🔴 CRITICAL: OTA JSON Format

**ota.json ДОЛЖЕН содержать firmware + websocket секции:**

```json
{
    "server_time": {"timestamp": 1786219600000, "timezone_offset": 180},
    "firmware": {"version": "X.Y.Z", "url": "http://..."},
    "websocket": {"url": "wss://api.tenclass.net/xiaozhi/v1/", "token": ""}
}
```

**ЗАПРЕЩЕНО:**
- ❌ Пустые секции `"websocket": {}` — стирает NVS конфиг
- ❌ Убирать websocket секцию — устройство не может подключиться к серверу
- ❌ Добавлять mqtt/activation секции — если не знаешь точные значения

**Почему:** устройство парсит ВСЕ секции из ota.json и перезаписывает NVS.
Пустой `"websocket": {}` стирает сохранённый websocket-конфиг → устройство
не может подключиться к серверу → "Поиск доступного сервиса" → БЕСПОЛЕЗНОЕ
устройство у клиента.

**Проверка перед deploy:**
```bash
# Должны быть РОВНО три секции: server_time, firmware, websocket
python3 -c "
import json, sys
data = json.load(open('/home/openclaw/bips-ota/ota.json'))
required = {'server_time', 'firmware', 'websocket'}
assert set(data.keys()) == required, f'ОШИБКА: ключи {set(data.keys())}, нужны {required}'
assert 'version' in data['firmware'], 'Нет firmware.version'
assert 'url' in data['firmware'], 'Нет firmware.url'
assert data['websocket']['url'].startswith('wss://'), 'websocket.url должен начинаться с wss://'
print('OK — server_time + firmware + websocket')
"
```

---

## 🔴 CRITICAL: Tests Before OTA

**ПЕРЕД ЛЮБЫМ OTA deploy ОБЯЗАТЕЛЬНО:**

```bash
cd ~/projects/bips-v3-firmware
source ~/esp-idf/export.sh
idf.py build                              # Сборка ДОЛЖНА пройти без ошибок
python3 scripts/tests/test_build.py       # ВСЕ тесты ДОЛЖНЫ пройти (51/51)
```

**Если хоть один тест упал — OTA НЕ ЗАЛИВАЕМ. Чиним сначала.**

---

## 🔴 CRITICAL: Version Bumping

Версия задаётся в `CMakeLists.txt`:
```cmake
set(PROJECT_VER "X.Y.Z")
```

**Правила:**
- Патч (bugfix): 4.1.3 → 4.1.4
- Минор (new feature): 4.1.4 → 4.2.0
- Мажор (breaking change): 4.1.4 → 5.0.0

**OTA проверяет версию:** если ota.json версия <= текущей → не обновляет.
Всегда инкрементируем.

---

## 🔴 CRITICAL: Git Before OTA

```bash
git add -A
git commit -m "vX.Y.Z: краткое описание"
TOKEN=$(python3 ~/.config/gh-app/get_token.py)
git remote set-url origin "https://x-access-token:${TOKEN}@github.com/Vitalymt/bips-firmware.git"
git push origin bips-v3
```

---

## 🟡 HARDWARE RULES

### GPIO43 (Touch Button) — НЕ RTC пин

На ESP32-S3 `ext0` wakeup работает ТОЛЬКО для RTC GPIO (0-21).
GPIO43 — обычный пин. Используем `gpio_wakeup` API:

```cpp
gpio_wakeup_enable(GPIO_NUM_43, GPIO_INTR_HIGH_LEVEL);
esp_sleep_enable_gpio_wakeup();  // НЕТ аргументов в IDF 6.x!
```

### C++26 Compliance

ESP-IDF 6.x использует C++26. Правила:
- `volatile int x; x++` → ОШИБКА. Используем `std::atomic<int>`
- Нужен `#include <atomic>`

### Sleep Task Stack

Минимум 3072 байт для sleep task (после `esp_light_sleep_start()` стек используется).

### Kconfig Alphabetical Order

Board entries в `main/Kconfig.projbuild` ДОЛЖНЫ быть в алфавитном порядке.
Тест `test_board_menu_is_sorted_and_matches_cmake` проверяет это.

### Manufacturer Subdirectory

Если `config.json` содержит `"manufacturer": "xxx"`, папка платы ДОЛЖНА быть:
```
main/boards/xxx/board-name/
```
НЕ `main/boards/board-name/`. Тест проверяет.

---

## 🟡 OTA SERVER

**Сервер:** `http://158.160.216.45:8099` (Python HTTP behind nginx)
**OTA файлы:** `/home/openclaw/bips-ota/`
**ota.json читается КАЖДЫЙ раз при проверке обновлений**

**Deploy checklist:**
1. `idf.py build` — без ошибок
2. `python3 scripts/tests/test_build.py` — все зелёные
3. `cp build/xiaozhi.bin /home/openclaw/bips-ota/xiaozhi.bin`
4. Записать o.json — ТОЛЬКО firmware секция
5. Проверить `curl http://158.160.216.45/bips-ota/ota.json`
6. `git commit + push`

---

## 🟡 KNOWN ISSUES TO WATCH

### TTP223 + Speaker Proximity
Если динамик близко к TTP223 — ложные срабатывания.
Решение: медная фольга между динамиком и сенсором, заземлённая на GND.

### Activation Flow
Устройство активируется через xiaozhi.me. Наш OTA-сервер НЕ отдаёт
activation/websocket/mqtt секции — они сохранены в NVS после активации.
НЕ СТИРАТЬ.

### WiFi Config Mode
Двойной клик boot → WiFi-конфиг. Через веб-интерфейс можно менять:
WiFi, OTA URL, Tavily API key.

---

## 📋 CHANGELOG

| Version | Date | Changes |
|---------|------|---------|
| 4.1.5 | 2026-08-08 | Volume buttons, std::atomic, sleep stack 3072 |
| 4.1.4 | 2026-08-08 | Boot double-click→WiFi config, Kconfig sorting, manufacturer subdir |
| 4.1.3 | 2026-08-08 | GPIO43 gpio_wakeup fix (ext0 doesn't work for non-RTC pins) |
| 4.1.2 | 2026-08-08 | Sleep task (was blocking timer callback) |
| 4.1.1 | 2026-08-07 | Touch debounce 2s |
| 4.1.0 | 2026-08-07 | Power save (display off 60s, light sleep 300s) |
| 4.0.9 | 2026-08-07 | OTA URL fix (GitHub→direct HTTP) |
| 4.0.8 | 2026-08-07 | Tavily web search tool |
| 4.0.7 | 2026-08-07 | Touch button debounce |
| 4.0.6 | 2026-08-07 | NTP Moscow timezone |
| 4.0.5 | 2026-08-07 | Initial BIPS v3 firmware |
