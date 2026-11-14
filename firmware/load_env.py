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
