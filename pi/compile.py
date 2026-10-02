import os
import subprocess

SCD4X = "embedded-i2c-scd4x"
BUILD = "build"  # object files go here

# Our code, compiled as C++
CPP_SOURCES = ["main.cpp", "control.cpp", "relay.cpp", "net.cpp"]

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


def obj_path(src):
    # embedded-i2c-scd4x/scd4x_i2c.c -> build/scd4x_i2c.o
    return os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".o")


# Compile each file to an object file (-c = compile only, don't link)
for src in C_SOURCES:
    subprocess.run(["gcc", "-Wall", "-I" + SCD4X, "-c", src, "-o", obj_path(src)], check=True)
for src in CPP_SOURCES:
    subprocess.run(["g++", "-Wall", "-std=c++20", "-I" + SCD4X, "-c", src, "-o", obj_path(src)], check=True)

# Link with g++ so the C++ standard library is included
objects = [obj_path(src) for src in C_SOURCES + CPP_SOURCES]
subprocess.run(["g++", "-o", "thermostat_control", *objects, "-lgpiod"], check=True)

subprocess.run(["./thermostat_control"], check=True)
