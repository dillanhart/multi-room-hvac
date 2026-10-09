"""
Build and run the unit tests on the dev machine (not the Pi).

    python3 tests/run_tests.py                          build + run everything
    python3 tests/run_tests.py --gtest_filter='TestSwitch.*'   extra args go to the test binary

Two test binaries, because relay.cpp and fake_relay.cpp define the same functions:
  unit_tests   pi/ logic + net.cpp, linked against fake_relay.cpp instead of relay.cpp
  relay_tests  the real relay.cpp, linked against fake_gpiod.cpp instead of libgpiod
No GPIO, I2C or libgpiod is needed. tests/gpiod/gpiod.h is a copy of the Pi's
libgpiod v2 header (LGPL-2.1-or-later), so relay.cpp compiles here.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PI = os.path.join(HERE, "..", "pi")
GTEST = os.path.join(HERE, "googletest", "googletest")
BUILD = os.path.join(HERE, "build")

# binary name -> (code under test from pi/, test files and fakes from tests/)
BINARIES = {
    "unit_tests": (["control.cpp", "validation.cpp", "net.cpp"],
                   ["fake_relay.cpp", "test_validation.cpp", "test_control.cpp", "test_net.cpp"]),
    "relay_tests": (["relay.cpp"],
                    ["fake_gpiod.cpp", "test_relay.cpp"]),
}

CXXFLAGS = ["-Wall", "-std=c++20", "-g", "-I" + PI, "-I" + HERE, "-I" + os.path.join(HERE, "gpiod"),
            "-isystem", os.path.join(GTEST, "include")]

os.makedirs(BUILD, exist_ok=True)

if not os.path.exists(os.path.join(GTEST, "src")):
    sys.exit("googletest is missing: run  git submodule update --init tests/googletest")


def obj_path(src):
    return os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".o")


def compile(src, flags):
    subprocess.run(["g++", *flags, "-c", src, "-o", obj_path(src)], check=True)


# googletest itself: slow to compile and never changes, so build it once
# (delete tests/build/ to force a rebuild)
gtest_sources = [os.path.join(GTEST, "src", "gtest-all.cc"), os.path.join(GTEST, "src", "gtest_main.cc")]
for src in gtest_sources:
    if not os.path.exists(obj_path(src)):
        print("building", os.path.basename(src), "(first run only)")
        compile(src, ["-std=c++20", "-I" + GTEST, "-isystem", os.path.join(GTEST, "include")])

binaries = []
for name, (pi_sources, test_sources) in BINARIES.items():
    sources = [os.path.join(PI, s) for s in pi_sources] + [os.path.join(HERE, s) for s in test_sources]
    for src in sources:
        compile(src, CXXFLAGS)
    binary = os.path.join(BUILD, name)
    objects = [obj_path(src) for src in gtest_sources + sources]
    subprocess.run(["g++", "-o", binary, *objects, "-pthread"], check=True)
    binaries.append(binary)

# run every binary even if one fails; exit non-zero if any failed (for scripts / CI)
failed = False
for binary in binaries:
    print("\n=====", os.path.basename(binary), flush=True)
    failed |= subprocess.run([binary, *sys.argv[1:]]).returncode != 0
sys.exit(1 if failed else 0)
