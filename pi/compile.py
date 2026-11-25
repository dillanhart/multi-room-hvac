import glob
import os
import subprocess
import sys

SCD4X = "embedded-i2c-scd4x"
BUILD = "build"  # object files go here
ENV_FILE = "thermostat.env"  # NWS gridpoint + contact; not in git (see thermostat.env.example)

# Our code, compiled as C++
CPP_SOURCES = ["main.cpp", "control.cpp", "relay.cpp", "net.cpp", "data_store.cpp", "weather.cpp",
               "validation.cpp", "history.cpp", "thermostat.cpp", "api.cpp"]

# C code, compiled as C: our sensor wrapper and the Sensirion driver
C_SOURCES = [
    "sensor.c",
    SCD4X + "/scd4x_i2c.c",
    SCD4X + "/sensirion_i2c.c",
    SCD4X + "/sensirion_common.c",
    # Linux I2C implementation (the top-level sensirion_i2c_hal.c is an empty template)
    SCD4X + "/sample-implementations/linux_user_space/sensirion_i2c_hal.c",
]

os.makedirs(BUILD, exist_ok=True)

# Rebuild an object only if it's older than its source or any header. Coarse (any
# header change rebuilds everything) but simple, and it skips net.cpp, which pulls
# in the ~24k-line httplib.h and takes the longest by far on a Pi 3.
HEADERS = glob.glob("*.h") + glob.glob("third_party/*.h") + glob.glob(SCD4X + "/*.h")
newest_header = max(os.path.getmtime(h) for h in HEADERS)


def obj_path(src):
    # embedded-i2c-scd4x/scd4x_i2c.c -> build/scd4x_i2c.o
    return os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".o")


def stale(src):
    obj = obj_path(src)
    if not os.path.exists(obj):
        return True
    built = os.path.getmtime(obj)
    return built < os.path.getmtime(src) or built < newest_header


# Compile each file to an object file (-c = compile only, don't link)
for src in C_SOURCES:
    if stale(src):
        subprocess.run(["gcc", "-Wall", "-I" + SCD4X, "-c", src, "-o", obj_path(src)], check=True)
for src in CPP_SOURCES:
    if stale(src):
        print("compiling", src)
        subprocess.run(["g++", "-Wall", "-std=c++20", "-I" + SCD4X, "-c", src, "-o", obj_path(src)], check=True)

# Link with g++ so the C++ standard library is included.
# -lssl -lcrypto: HTTPS for the weather fetch; -pthread: the control/weather/API threads;
# -lsystemd: sd_notify (libsystemd-dev)
objects = [obj_path(src) for src in C_SOURCES + CPP_SOURCES]
subprocess.run(["g++", "-pthread", "-o", "thermostat_control", *objects, "-lgpiod", "-lssl", "-lcrypto",
                "-lsystemd"],
               check=True)


def load_env(path):
    # KEY=VALUE lines; blank lines and # comments skipped
    env = dict(os.environ)
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, value = line.split("=", 1)
                    env[key.strip()] = value.strip()
    return env


# --no-run: build only (used by deploy.sh build)
if "--no-run" not in sys.argv:
    subprocess.run(["./thermostat_control"], check=True, env=load_env(ENV_FILE))
