# BIPS Firmware

Прошивка для устройств BIPS — карманных AI-ассистентов на базе ESP32-S3.

## Устройства

| Устройство | Ветка | Текущая версия | Описание |
|------------|-------|----------------|----------|
| BIPS v3 | `bips-v3` | v4.0.4 | XH-S3E-AI, SH1106 OLED, сенсорная кнопка |
| BIPS v1 | `bips-v1` | — | Китайская коробочка |
| BIPS v2 | `bips-v2` | — | Бибс v2 |

## Облачные обновления (OTA)

Каждое устройство проверяет обновления при включении. URL обновлений зашит в прошивку и не меняется.

### URL обновлений

```
BIPS v1: https://raw.githubusercontent.com/Vitalymt/bips-firmware/bips-v1/ota.json
BIPS v2: https://raw.githubusercontent.com/Vitalymt/bips-firmware/bips-v2/ota.json
BIPS v3: https://raw.githubusercontent.com/Vitalymt/bips-firmware/bips-v3/ota.json
```

### Как работает OTA

```
Устройство включается
    ↓
Скачивает ota.json (версия + URL бинарника)
    ↓
Сравнивает версию (4.0.4 vs своя)
    ↓
Если новее → скачивает xiaozhi.bin → обновляется
Если такая же → пропускает
```

### Структура ota.json

```json
{
    "server_time": {
        "timestamp": 1786032000000,
        "timezone_offset": 180
    },
    "firmware": {
        "version": "4.0.4",
        "url": "https://github.com/Vitalymt/bips-firmware/releases/download/bips-v3/v4.0.4/xiaozhi.bin"
    }
}
```

## Разработка

### Требования

- ESP-IDF v5.4+
- Python 3.10+
- Git

### Локальная сборка

```bash
# 1. Клонировать репозиторий
git clone https://github.com/Vitalymt/bips-firmware.git
cd bips-firmware

# 2. Переключиться на ветку устройства
git checkout bips-v3

# 3. Активировать ESP-IDF
source ~/esp-idf/export.sh

# 4. Собрать
idf.py build

# 5. Прошить через USB
esptool.py --chip esp32s3 -b 460800 write-flash \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0xd000 build/ota_data_initial.bin \
    0x20000 build/xiaozhi.bin \
    0x800000 build/generated_assets.bin
```

### Прошивка через OTA

```bash
# 1. Собрать прошивку
idf.py build

# 2. Обновить версию в CMakeLists.txt
# set(PROJECT_VER "4.0.5")

# 3. Создать релиз на GitHub
git tag bips-v3/v4.0.5
git push origin bips-v3 --tags

# 4. Загрузить xiaozhi.bin как asset релиза
# (через web-интерфейс GitHub или gh CLI)

# 5. Обновить ota.json в ветке
# Изменить версию и URL на новый релиз
git add ota.json
git commit -m "Update OTA to v4.0.5"
git push origin bips-v3
```

## Процесс релиза

### 1. Подготовка

```bash
# Внести изменения
# Протестировать локально через USB
# Обновить версию в CMakeLists.txt
```

### 2. Сборка

```bash
idf.py build
```

### 3. Коммит и тег

```bash
git add -A
git commit -m "v4.0.5: описание изменений"
git tag bips-v3/v4.0.5
git push origin bips-v3 --tags
```

### 4. Создание релиза на GitHub

1. Перейти в Releases → Create new release
2. Выбрать тег `bips-v3/v4.0.5`
3. Загрузить `build/xiaozhi.bin` как asset
4. Опубликовать релиз

### 5. Обновление OTA

```bash
# Обновить ota.json в ветке
cat > ota.json << EOF
{
    "server_time": {"timestamp": $(date +%s)000, "timezone_offset": 180},
    "firmware": {
        "version": "4.0.5",
        "url": "https://github.com/Vitalymt/bips-firmware/releases/download/bips-v3/v4.0.5/xiaozhi.bin"
    }
}
EOF

git add ota.json
git commit -m "OTA: v4.0.5"
git push origin bips-v3
```

### 6. Проверка

```bash
# Проверить что ota.json доступен
curl https://raw.githubusercontent.com/Vitalymt/bips-firmware/bips-v3/ota.json

# Проверить что бинарник доступен
curl -I https://github.com/Vitalymt/bips-firmware/releases/download/bips-v3/v4.0.5/xiaozhi.bin
```

## Структура репозитория

```
bips-firmware/
├── .github/
│   └── workflows/
│       └── build.yml          ← GitHub Actions (опционально)
├── main/
│   └── boards/
│       └── bips-v3/           ← код платы
│           ├── bips_v3_board.cc
│           ├── config.h
│           └── config.json
├── managed_components/        ← ESP-IDF компоненты
├── CMakeLists.txt             ← PROJECT_VER = версия
├── sdkconfig                  ← настройки (OTA URL, OLED, язык)
├── ota.json                   ← текущая версия OTA
├── README.md                  ← этот файл
└── docs/
    └── ARCHITECTURE.md        ← подробная архитектура
```

## Версионирование

Формат: `MAJOR.MINOR.PATCH`

- **MAJOR** — критические изменения, новое железо
- **MINOR** — новые функции, улучшения
- **PATCH** — исправления багов

Примеры:
- `4.0.4` → `4.0.5` — исправление бага
- `4.0.4` → `4.1.0` — новая функция (Tavily поиск)
- `4.0.4` → `5.0.0` — новое железо (BIPS v4)

## Поддержка пользователей

### Пользователь сообщает о проблеме

1. Узнать версию прошивки (показывает на экране)
2. Проверить OTA: `curl https://raw.githubusercontent.com/.../ota.json`
3. Если есть новая версия → сказать перезагрузить устройство
4. Если нет → исправить, задеплоить, сказать перезагрузить
5. Если устройство не обновляется → прошивка через USB

### Пользователь хочет кастомизацию

- Дополнительный функционал → платная услуга (500₽+)
- Изменение OTA URL → через WiFi config Advanced tab
- Свой сервер обновлений → можно указать свой URL

## Безопасность

- OTA обновления через HTTP (не HTTPS) — для простоты
- Версионирование предотвращает откат на старую версию
- Пользователь может указать свой OTA URL

### Рекомендации

- Не публиковать敏感 данные в репозитории
- Использовать `.env` для API ключей
- Проверять код перед коммитом

## GitHub Actions (опционально)

Автоматическая сборка при пуше в ветку:

```yaml
# .github/workflows/build.yml
name: Build Firmware

on:
  push:
    branches: [bips-v1, bips-v2, bips-v3]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Setup ESP-IDF
        uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.4
          target: esp32s3
      - name: Build
        run: |
          source ~/esp-idf/export.sh
          idf.py build
      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: firmware-${{ github.ref_name }}
          path: build/xiaozhi.bin
```

## Контакты

- Telegram: @hitdata
- Avito: Виталий и Екатерина

## Лицензия

Проприетарная. Все права защищены.
