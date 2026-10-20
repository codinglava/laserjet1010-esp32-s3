Import("env")
from pathlib import Path

root = Path(env["PROJECT_DIR"])
env_path = root / ".env"
if not env_path.exists():
    raise SystemExit("copy .env.example to .env and set WIFI_SSID and WIFI_PASS")

vals = {}
for line in env_path.read_text().splitlines():
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
        continue
    key, value = line.split("=", 1)
    vals[key.strip()] = value.strip().strip('"').strip("'")

ssid, password = vals.get("WIFI_SSID", ""), vals.get("WIFI_PASS", "")
if not ssid or not password:
    raise SystemExit(".env needs WIFI_SSID and WIFI_PASS")
if any(c in ssid + password for c in "\r\n"):
    raise SystemExit("WIFI_SSID and WIFI_PASS must be single-line")

def esc(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')

(root / "src" / "wifi_secret.h").write_text(
    f'#pragma once\n#define WIFI_SSID "{esc(ssid)}"\n#define WIFI_PASS "{esc(password)}"\n'
)
