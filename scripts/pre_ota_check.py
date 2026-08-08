#!/usr/bin/env python3
"""Pre-OTA safety check for BIPS firmware.
Run BEFORE any OTA deploy. Exits non-zero on failure.

Usage: python3 scripts/pre_ota_check.py
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OTA_JSON = Path("/home/openclaw/bips-ota/ota.json")
OTA_BIN = Path("/home/openclaw/bips-ota/xiaozhi.bin")

RED = "\033[91m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
RESET = "\033[0m"

errors = []
warnings = []


def check(name, ok, msg=""):
    if ok:
        print(f"  {GREEN}✓{RESET} {name}")
    else:
        print(f"  {RED}✗{RESET} {name}: {msg}")
        errors.append(f"{name}: {msg}")


def warn(name, msg):
    print(f"  {YELLOW}⚠{RESET} {name}: {msg}")
    warnings.append(f"{name}: {msg}")


print(f"\n{'='*60}")
print("BIPS Pre-OTA Safety Check")
print(f"{'='*60}\n")

# 1. Check ota.json format
print("1. OTA JSON format:")
if OTA_JSON.exists():
    try:
        data = json.loads(OTA_JSON.read_text())
        check("ota.json is valid JSON", True)
        check("Has 'firmware' key", "firmware" in data,
              f"keys are: {list(data.keys())}")
        if "firmware" in data:
            fw = data["firmware"]
            check("Has firmware.version", "version" in fw)
            check("Has firmware.url", "url" in fw)
            check("Only 'firmware' section (no websocket/mqtt/activation)",
                  set(data.keys()) == {"firmware"},
                  f"extra sections: {set(data.keys()) - {'firmware'}}")
    except json.JSONDecodeError as e:
        check("ota.json is valid JSON", False, str(e))
else:
    check("ota.json exists", False, "file not found")

# 2. Check ota.bin exists and is recent
print("\n2. OTA binary:")
if OTA_BIN.exists():
    size = OTA_BIN.stat().st_size
    check(f"ota.bin exists ({size:,} bytes)", size > 100_000,
          f"file too small: {size}")
else:
    check("ota.bin exists", False, "file not found")

# 3. Check version consistency
print("\n3. Version consistency:")
cmake_ver = None
ota_ver = None
cmake_file = ROOT / "CMakeLists.txt"
if cmake_file.exists():
    for line in cmake_file.read_text().splitlines():
        if "PROJECT_VER" in line and "set(" in line:
            cmake_ver = line.split('"')[1]
            break
    check(f"CMakeLists.txt has version: {cmake_ver}", cmake_ver is not None)
if "firmware" in data:
    ota_ver = data["firmware"]["version"]
if cmake_ver and ota_ver:
    check(f"OTA version matches CMake ({ota_ver} == {cmake_ver})",
          ota_ver == cmake_ver,
          f"OTA={ota_ver}, CMake={cmake_ver}")

# 4. Run build
print("\n4. Build check:")
bash_cmd = "source ~/esp-idf/export.sh 2>/dev/null && idf.py build"
r = subprocess.run(["bash", "-c", bash_cmd], cwd=ROOT,
                    capture_output=True, text=True, timeout=300)
check("idf.py build succeeded", r.returncode == 0,
      "see build log for errors")

# 5. Run tests
print("\n5. Test suite:")
r = subprocess.run(
    [sys.executable, "scripts/tests/test_build.py"],
    cwd=ROOT, capture_output=True, text=True, timeout=60
)
check("All tests passed", r.returncode == 0,
      f"exit code {r.returncode}" + (f"\n{r.stderr[-200:]}" if r.stderr else ""))

# 6. Git status
print("\n6. Git status:")
r = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                    capture_output=True, text=True)
if r.stdout.strip():
    warn("Uncommitted changes", f"{len(r.stdout.strip().splitlines())} files")

# Summary
print(f"\n{'='*60}")
if errors:
    print(f"{RED}BLOCKED — {len(errors)} error(s). DO NOT DEPLOY OTA.{RESET}")
    for e in errors:
        print(f"  {RED}✗{RESET} {e}")
    sys.exit(1)
elif warnings:
    print(f"{YELLOW}PASSED with {len(warnings)} warning(s).{RESET}")
    for w in warnings:
        print(f"  {YELLOW}⚠{RESET} {w}")
else:
    print(f"{GREEN}ALL CLEAR — safe to deploy OTA.{RESET}")
sys.exit(0)
