#!/usr/bin/env python3
"""Bump firmware version in all required places and optionally commit.

Usage:
    python3 scripts/bump_version.py 4.1.6          # bump + commit
    python3 scripts/bump_version.py 4.1.6 --dry    # show changes only
    python3 scripts/bump_version.py 4.1.6 --ota    # bump + commit + update ota.json on server

Places that hold the version:
    1. CMakeLists.txt — set(PROJECT_VER "X.Y.Z")
    2. ota.json on server — firmware.version
    3. Git tag (optional)
"""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE_FILE = ROOT / "CMakeLists.txt"
OTA_JSON = Path("/home/openclaw/bips-ota/ota.json")
OTA_PROXY_CONFIG = Path("/home/openclaw/bips-ota/ota_proxy_config.json")

RED = "\033[91m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
RESET = "\033[0m"


def get_current_version():
    text = CMAKE_FILE.read_text()
    m = re.search(r'set\(PROJECT_VER\s+"(\d+\.\d+\.\d+)"\)', text)
    return m.group(1) if m else None


def update_cmake(new_ver):
    text = CMAKE_FILE.read_text()
    new_text = re.sub(
        r'(set\(PROJECT_VER\s+)("\d+\.\d+\.\d+")',
        rf'\g<1>"{new_ver}"',
        text
    )
    CMAKE_FILE.write_text(new_text)
    return new_text != text


def update_ota_json(new_ver):
    if not OTA_JSON.exists():
        return False
    data = json.loads(OTA_JSON.read_text())
    old = data["firmware"]["version"]
    data["firmware"]["version"] = new_ver
    OTA_JSON.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
    return old != new_ver


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <version> [--dry] [--ota]")
        print(f"  Current version: {get_current_version()}")
        sys.exit(1)

    new_ver = sys.argv[1]
    dry = "--dry" in sys.argv
    update_ota = "--ota" in sys.argv

    if not re.match(r'^\d+\.\d+\.\d+$', new_ver):
        print(f"{RED}Invalid version format: {new_ver} (expected X.Y.Z){RESET}")
        sys.exit(1)

    old_ver = get_current_version()
    if not old_ver:
        print(f"{RED}Could not read current version from CMakeLists.txt{RESET}")
        sys.exit(1)

    print(f"Version: {old_ver} → {new_ver}\n")

    if dry:
        print(f"  [DRY] CMakeLists.txt: {old_ver} → {new_ver}")
        if update_ota:
            print(f"  [DRY] ota.json: {old_ver} → {new_ver}")
        print(f"\n{YELLOW}Dry run — no changes made.{RESET}")
        return

    # 1. CMakeLists.txt
    if update_cmake(new_ver):
        print(f"  {GREEN}✓{RESET} CMakeLists.txt: {old_ver} → {new_ver}")
    else:
        print(f"  {YELLOW}⚠{RESET} CMakeLists.txt: no change (already {new_ver}?)")

    # 2. ota.json (optional)
    if update_ota:
        if update_ota_json(new_ver):
            print(f"  {GREEN}✓{RESET} ota.json: {old_ver} → {new_ver}")
        else:
            print(f"  {YELLOW}⚠{RESET} ota.json: no change or file missing")

    # 3. Git commit
    files = [str(CMAKE_FILE)]
    if update_ota:
        files.append(str(OTA_JSON))
    subprocess.run(["git", "add"] + files, cwd=ROOT)
    r = subprocess.run(
        ["git", "commit", "-m", f"Bump version {old_ver} → {new_ver}"],
        cwd=ROOT, capture_output=True, text=True
    )
    if r.returncode == 0:
        print(f"  {GREEN}✓{RESET} Git commit: {r.stdout.strip().split(chr(10))[0]}")
    else:
        print(f"  {YELLOW}⚠{RESET} Git commit: {r.stderr.strip()}")

    print(f"\n{GREEN}Done!{RESET} Version bumped to {new_ver}")
    print(f"Next: git push origin bips-v3 && python3 scripts/pre_ota_check.py")


if __name__ == "__main__":
    main()
