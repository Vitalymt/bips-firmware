# BIPS Firmware — Architecture & Cloud Update Strategy

## Overview

This document describes the firmware management architecture for BIPS devices.
Each device type has its own firmware branch, OTA endpoint, and release cycle.

---

## 1. Repository Structure

### GitHub Repository

```
github.com/your-username/bips-firmware
```

### Branches

```
main                    ← stable, always working, merge target
├── bips-v1             ← Chinese box (stock XiaoZhi + minor mods)
├── bips-v2             ← BIPS v2 (if different hardware)
├── bips-v3             ← BIPS v3 (XH-S3E-AI, current device)
└── dev                 ← experimental, not for production
```

### Tags (releases)

```
bips-v3/v4.0.4          ← release tag for BIPS v3
bips-v3/v4.1.0          ← next release
bips-v1/v1.0.0          ← release tag for BIPS v1
```

### Directory Structure (per branch)

```
bips-firmware/
├── main/
│   └── boards/
│       └── bips-v3/          ← board-specific code
│           ├── bips_v3_board.cc
│           ├── config.h
│           └── config.json
├── managed_components/       ← ESP-IDF components (auto-managed)
├── CMakeLists.txt            ← PROJECT_VER = version
├── sdkconfig                 ← CONFIG_OTA_URL, CONFIG_OLED_*, etc.
├── BIPS_V3_STATUS.md         ← current status
└── docs/
    ├── ARCHITECTURE.md       ← this file
    └── OTA_SERVER.md         ← OTA server setup
```

---

## 2. OTA Server Architecture

### Current Setup (single server)

```
VM: 158.160.216.45
├── nginx: /bips-ota/ → proxy_pass http://127.0.0.1:8099/
└── Python backend (port 8099)
    ├── ota.json              ← version + URL
    └── xiaozhi.bin           ← firmware binary
```

### Multi-device Setup

```
VM: 158.160.216.45
├── /bips-v1/
│   ├── ota.json              ← {"firmware": {"version": "1.0.0", ...}}
│   └── xiaozhi.bin
├── /bips-v2/
│   ├── ota.json
│   └── xiaozhi.bin
├── /bips-v3/
│   ├── ota.json
│   └── xiaozhi.bin
└── /ota-server/
    ├── server.py             ← unified OTA server
    └── deploy.sh             ← deployment script
```

### OTA URL per Device

Each device has its own `CONFIG_OTA_URL` in `sdkconfig`:

```
BIPS v1: CONFIG_OTA_URL="http://158.160.216.45/bips-v1/ota.json"
BIPS v2: CONFIG_OTA_URL="http://158.160.216.45/bips-v2/ota.json"
BIPS v3: CONFIG_OTA_URL="http://158.160.216.45/bips-v3/ota.json"
```

Users can also change OTA URL via WiFi config Advanced tab.

---

## 3. Version Management

### Version Format

```
MAJOR.MINOR.PATCH

MAJOR = breaking changes, new hardware support
MINOR = new features, improvements
PATCH = bug fixes, display fixes, etc.
```

### Current Versions

| Device | Current | Notes |
|--------|---------|-------|
| BIPS v3 | 4.0.4 | Display fix, power save, NTP, Tavily |
| BIPS v1 | — | Not started |
| BIPS v2 | — | Not started |

### Version in Code

`CMakeLists.txt`:
```cmake
set(PROJECT_VER "4.0.4")
```

`ota.json`:
```json
{
    "firmware": {
        "version": "4.0.4",
        "url": "http://158.160.216.45/bips-v3/xiaozhi.bin"
    }
}
```

**Critical:** `PROJECT_VER` in CMakeLists.txt MUST match `version` in ota.json.
If they don't match, device will either skip update or loop.

---

## 4. Build & Deploy Workflow

### Local Development (you)

```bash
# 1. Enter firmware directory
cd ~/projects/bips-v3-firmware

# 2. Activate ESP-IDF
source ~/esp-idf/export.sh 2>/dev/null

# 3. Make changes (edit files)

# 4. Build
idf.py build

# 5. Test locally (USB flash)
esptool.py --chip esp32s3 -b 460800 write-flash \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0xd000 build/ota_data_initial.bin \
    0x20000 build/xiaozhi.bin \
    0x800000 build/generated_assets.bin

# 6. Deploy to OTA
cp build/xiaozhi.bin /home/openclaw/bips-ota/xiaozhi.bin
# Update ota.json version

# 7. Git commit + push
git add -A
git commit -m "v4.0.4: description"
git tag bips-v3/v4.0.4
git push origin bips-v3 --tags
```

### GitHub Actions (automated)

Create `.github/workflows/build-and-deploy.yml`:

```yaml
name: Build and Deploy OTA

on:
  push:
    tags:
      - 'bips-v*/v*'

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
      
      - name: Extract version
        id: version
        run: echo "version=$(grep PROJECT_VER CMakeLists.txt | grep -o '[0-9.]*')" >> $GITHUB_OUTPUT
      
      - name: Create OTA JSON
        run: |
          DEVICE=$(echo ${{ github.ref_name }} | cut -d'/' -f1)
          cat > ota.json << EOF
          {
              "server_time": {"timestamp": $(date +%s)000, "timezone_offset": 180},
              "firmware": {
                  "version": "${{ steps.version.outputs.version }}",
                  "url": "http://158.160.216.45/${DEVICE}/xiaozhi.bin"
              }
          }
          EOF
      
      - name: Deploy to server
        uses: appleboy/scp-action@v0.1.7
        with:
          host: 158.160.216.45
          username: openclaw
          key: ${{ secrets.SSH_KEY }}
          source: "build/xiaozhi.bin,ota.json"
          target: "/home/openclaw/ota/${{ github.ref_name }}/"
      
      - name: Restart OTA server
        uses: appleboy/ssh-action@v1.0.3
        with:
          host: 158.160.216.45
          username: openclaw
          key: ${{ secrets.SSH_KEY }}
          script: systemctl --user restart ota-server
```

---

## 5. OTA Server Setup

### Unified Server (handles all devices)

```python
#!/usr/bin/env python3
"""Unified OTA server for all BIPS devices."""
import http.server
import json
import os
from pathlib import Path

OTA_ROOT = Path("/home/openclaw/ota")

class OTAHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        # Parse path: /bips-v3/ota.json or /bips-v3/xiaozhi.bin
        parts = self.path.strip("/").split("/")
        if len(parts) != 2:
            self.send_error(404)
            return
        
        device, filename = parts
        filepath = OTA_ROOT / device / filename
        
        if not filepath.exists():
            self.send_error(404)
            return
        
        if filename == "ota.json":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(filepath.read_bytes())
        elif filename == "xiaozhi.bin":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.end_headers()
            self.wfile.write(filepath.read_bytes())
        else:
            self.send_error(404)
    
    def log_message(self, format, *args):
        print(f"[OTA] {self.client_address[0]} {args[0]}")

if __name__ == "__main__":
    server = http.server.HTTPServer(("0.0.0.0", 8099), OTAHandler)
    print("OTA server running on port 8099")
    server.serve_forever()
```

### Nginx Config

```nginx
server {
    listen 80;
    server_name 158.160.216.45;
    
    # BIPS v1
    location /bips-v1/ {
        proxy_pass http://127.0.0.1:8099/;
    }
    
    # BIPS v2
    location /bips-v2/ {
        proxy_pass http://127.0.0.1:8099/;
    }
    
    # BIPS v3
    location /bips-v3/ {
        proxy_pass http://127.0.0.1:8099/;
    }
}
```

---

## 6. Release Process

### Step-by-step

1. **Make changes** in the device branch (e.g., `bips-v3`)
2. **Test locally** via USB flash
3. **Update version** in `CMakeLists.txt` (e.g., `4.0.4` → `4.0.5`)
4. **Build** with `idf.py build`
5. **Deploy to OTA** (copy binary + update ota.json)
6. **Git commit + tag** (e.g., `bips-v3/v4.0.5`)
7. **Push to GitHub** (triggers CI if configured)
8. **Verify** device updates via OTA

### Hotfix Process

For critical bugs (device bricked, security issue):

1. **Fix** in device branch
2. **Build** and test locally
3. **Deploy** to OTA immediately
4. **Git commit + tag** (e.g., `bips-v3/v4.0.5-hotfix1`)
5. **Push** to GitHub

---

## 7. User Support

### When user has issues

1. **Check version:** User says "version 3.9.0" → check if OTA has newer
2. **Check OTA:** `curl http://158.160.216.45/bips-v3/ota.json` → verify version
3. **Check device:** Device should auto-update on reboot
4. **If stuck:** User can flash via USB (provide flash.zip)

### User-facing OTA URL

Users can change OTA URL via WiFi config Advanced tab:
- Default: `http://158.160.216.45/bips-v3/ota.json`
- Custom: User can point to their own server

### Support workflow

```
User reports issue
↓
Check device version (ask user or check logs)
↓
Is there a newer version on OTA?
├── Yes → Tell user to reboot device
└── No → Fix issue, deploy to OTA, tell user to reboot
↓
If device stuck → Provide USB flash instructions
```

---

## 8. Security Considerations

### OTA Security

- **No authentication** on OTA server (public endpoint)
- **No encryption** (HTTP, not HTTPS) — acceptable for non-sensitive devices
- **Version check** prevents downgrade attacks (device won't install older version)

### Future improvements

- **HTTPS** with Let's Encrypt certificate
- **Signed firmware** (verify binary signature before install)
- **User authentication** (per-device OTA URL with token)

---

## 9. Monitoring

### What to monitor

- **OTA server uptime** (is it responding?)
- **Update success rate** (are devices updating successfully?)
- **Version distribution** (what versions are devices running?)

### Simple monitoring script

```bash
#!/bin/bash
# Check OTA server health
for device in bips-v1 bips-v2 bips-v3; do
    response=$(curl -s -o /dev/null -w "%{http_code}" \
        "http://158.160.216.45/$device/ota.json")
    if [ "$response" != "200" ]; then
        echo "ALERT: $device OTA server down (HTTP $response)"
    fi
done
```

---

## 10. Quick Reference

### Build command

```bash
cd ~/projects/bips-v3-firmware && source ~/esp-idf/export.sh 2>/dev/null && idf.py build
```

### Deploy to OTA

```bash
cp build/xiaozhi.bin /home/openclaw/bips-ota/xiaozhi.bin
# Update ota.json version
```

### Git workflow

```bash
git add -A
git commit -m "v4.0.4: description"
git tag bips-v3/v4.0.4
git push origin bips-v3 --tags
```

### Check OTA status

```bash
curl http://158.160.216.45/bips-v3/ota.json
```

---

## Summary

| Component | Location | Purpose |
|-----------|----------|---------|
| GitHub repo | `github.com/your-username/bips-firmware` | Version control, history |
| Device branches | `bips-v1`, `bips-v2`, `bips-v3` | Per-device firmware |
| Release tags | `bips-v3/v4.0.4` | Version tracking |
| OTA server | `158.160.216.45` | Cloud updates |
| OTA endpoints | `/bips-v1/`, `/bips-v2/`, `/bips-v3/` | Per-device updates |
| Local build | `~/projects/bips-v3-firmware/` | Development |
| OTA files | `/home/openclaw/bips-ota/` | Deployment |

**Next steps:**
1. Create GitHub repo
2. Push current code to `bips-v3` branch
3. Set up multi-device OTA server
4. Configure GitHub Actions for automated deployment
