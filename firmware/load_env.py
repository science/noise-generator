# PlatformIO pre-build script: turn firmware/.env (gitignored) into string
# macros, so secrets and site-specific values (Bluetooth addresses, later
# Wi-Fi credentials) never land in this public repo. Same pattern as
# ~/dev/hot-tub-controller/esp32/load_env.py.
#
#   BT_SINK_ADDR=aa:bb:cc:dd:ee:ff   ->   -DBT_SINK_ADDR="aa:bb:cc:dd:ee:ff"
Import("env")  # noqa: F821 (provided by PlatformIO's SCons)
import os

env_file = os.path.join(env.get("PROJECT_DIR"), ".env")  # noqa: F821
if os.path.exists(env_file):
    with open(env_file) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                env.Append(CPPDEFINES=[(key.strip(), env.StringifyMacro(value.strip()))])  # noqa: F821
else:
    print("WARNING: firmware/.env not found - copy .env.example to .env and fill it in")

# espota (env app_ota): take the device IP and OTA password from .env too.
if env.get("PIOENV") == "app_ota" and os.path.exists(env_file):  # noqa: F821
    vals = {}
    with open(env_file) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                vals[k.strip()] = v.strip()
    if vals.get("NOISE_IP"):
        env.Replace(UPLOAD_PORT=vals["NOISE_IP"])  # noqa: F821
    if vals.get("OTA_PASSWORD"):
        env.Append(UPLOAD_FLAGS=["--auth=" + vals["OTA_PASSWORD"]])  # noqa: F821
