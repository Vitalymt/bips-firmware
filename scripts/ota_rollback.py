#!/usr/bin/env python3
"""Manage OTA firmware versions on the server — rollback, list, backup.

Usage:
    python3 scripts/ota_rollback.py list        # show available versions
    python3 scripts/ota_rollback.py backup      # save current binary before deploy
    python3 scripts/ota_rollback.py restore 4.1.3  # rollback to specific version
"""
import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

OTA_DIR = Path("/home/openclaw/bips-ota")
OTA_JSON = OTA_DIR / "ota.json"
OTA_BIN = OTA_DIR / "xiaozhi.bin"
BACKUP_DIR = OTA_DIR / "backups"
MAX_BACKUPS = 3

GREEN = "\033[92m"
YELLOW = "\033[93m"
RED = "\033[91m"
RESET = "\033[0m"


def get_current_version():
    if not OTA_JSON.exists():
        return None
    data = json.loads(OTA_JSON.read_text())
    return data.get("firmware", {}).get("version")


def list_backups():
    BACKUP_DIR.mkdir(exist_ok=True)
    backups = sorted(BACKUP_DIR.glob("xiaozhi-*.bin"), reverse=True)
    return backups


def cmd_list():
    current = get_current_version()
    print(f"Current OTA version: {GREEN}{current}{RESET}\n")
    backups = list_backups()
    if not backups:
        print("No backups available.")
        return
    print("Available backups:")
    for b in backups:
        size = b.stat().st_size
        mtime = datetime.fromtimestamp(b.stat().st_mtime).strftime("%Y-%m-%d %H:%M")
        ver = b.stem.replace("xiaozhi-", "")
        marker = " ← current" if ver == current else ""
        print(f"  {ver}  {size:>10,} bytes  {mtime}{marker}")


def cmd_backup():
    current = get_current_version()
    if not current:
        print(f"{RED}Cannot read current version from ota.json{RESET}")
        return False

    BACKUP_DIR.mkdir(exist_ok=True)
    dest = BACKUP_DIR / f"xiaozhi-{current}.bin"

    if dest.exists():
        print(f"{YELLOW}Backup for {current} already exists, skipping.{RESET}")
        return True

    shutil.copy2(OTA_BIN, dest)
    print(f"{GREEN}✓{RESET} Backed up {current} → {dest.name}")

    # Clean old backups
    backups = sorted(BACKUP_DIR.glob("xiaozhi-*.bin"), reverse=True)
    for old in backups[MAX_BACKUPS:]:
        old.unlink()
        print(f"  Removed old backup: {old.name}")

    return True


def cmd_restore(version):
    BACKUP_DIR.mkdir(exist_ok=True)
    backup = BACKUP_DIR / f"xiaozhi-{version}.bin"
    if not backup.exists():
        print(f"{RED}No backup found for version {version}{RESET}")
        print("Available:")
        cmd_list()
        sys.exit(1)

    # Backup current before restoring
    current = get_current_version()
    if current and current != version:
        cmd_backup()

    # Restore binary
    shutil.copy2(backup, OTA_BIN)
    print(f"{GREEN}✓{RESET} Restored binary: {version}")

    # Update ota.json
    if OTA_JSON.exists():
        data = json.loads(OTA_JSON.read_text())
        data["firmware"]["version"] = version
        OTA_JSON.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
        print(f"{GREEN}✓{RESET} Updated ota.json: firmware.version = {version}")

    print(f"\n{GREEN}Rollback to {version} complete.{RESET}")
    print("Device will update on next OTA check.")


def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python3 ota_rollback.py list")
        print("  python3 ota_rollback.py backup")
        print("  python3 ota_rollback.py restore <version>")
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "list":
        cmd_list()
    elif cmd == "backup":
        cmd_backup()
    elif cmd == "restore":
        if len(sys.argv) < 3:
            print(f"{RED}Specify version: restore 4.1.3{RESET}")
            sys.exit(1)
        cmd_restore(sys.argv[2])
    else:
        print(f"{RED}Unknown command: {cmd}{RESET}")
        sys.exit(1)


if __name__ == "__main__":
    main()
