#!/bin/bash
# BIPS v3 Firmware OTA Publisher
# Usage: ./publish.sh [version]
# Example: ./publish.sh 3.0.1

set -e

VERSION=${1:-"3.0.0"}
REPO="Vitalymt/bips-firmware"
FIRMWARE_DIR="$HOME/projects/xiaozhi-esp32-vanilla"
OTA_DIR="$HOME/bips-firmware"

echo "=== BIPS v3 OTA Publisher ==="
echo "Version: $VERSION"
echo ""

# Step 1: Build firmware
echo "[1/5] Building firmware..."
cd "$FIRMWARE_DIR"
source ~/esp-idf/export.sh 2>/dev/null
idf.py build 2>&1 | tail -3

# Step 2: Copy firmware binary
echo "[2/5] Copying firmware binary..."
cp build/xiaozhi.bin "$OTA_DIR/xiaozhi.bin"

# Step 3: Update ota.json
echo "[3/5] Updating ota.json..."
cat > "$OTA_DIR/ota.json" << EOF
{
  "firmware": {
    "version": "$VERSION",
    "url": "https://github.com/$REPO/releases/download/v$VERSION/xiaozhi.bin"
  }
}
EOF

# Step 4: Git commit and push
echo "[4/5] Pushing to GitHub..."
cd "$OTA_DIR"
git add ota.json xiaozhi.bin
git commit -m "Release v$VERSION"
git tag "v$VERSION"
git push origin master --tags

# Step 5: Create GitHub release
echo "[5/5] Creating GitHub release..."
gh release create "v$VERSION" xiaozhi.bin \
    --repo "$REPO" \
    --title "BIPS v$VERSION" \
    --notes "BIPS v$VERSION firmware update"

echo ""
echo "=== Done! ==="
echo "OTA URL: https://raw.githubusercontent.com/$REPO/main/ota.json"
echo "Release: https://github.com/$REPO/releases/tag/v$VERSION"
echo ""
echo "Device will check for updates automatically on boot."
echo "To force update: hold BOOT button for 10 seconds during startup."
