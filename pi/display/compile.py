"""
Build (and run) the display program. Run from pi/display/:

    python3 compile.py             on the Pi: build, then run ./thermostat_display
    python3 compile.py --no-run    on the Pi: build only (./deploy.sh display-build)
    python3 compile.py --preview   on Ubuntu: build ./display_preview (no panel, touch
                                   or API) and run it: renders every screen to preview_out/

LVGL (~620 files, with ThorVG for the animated icons) is compiled once into
build/liblvgl.a, in parallel. A file is recompiled only when it's newer than its
object or lv_conf.h changed. After updating the lvgl submodule, delete build/.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys

LVGL = "lvgl"
BUILD = "build"

# program -> (our sources, extra libraries)
TARGETS = {
    "thermostat_display": (["display.cpp", "ui.cpp", "api_client.cpp", "touch.cpp", "display_logic.cpp"],
                           ["-lgpiod"]),
    # desktop check: no touch.cpp (libgpiod), no display.cpp (framebuffer)
    "display_preview": (["preview.cpp", "ui.cpp", "api_client.cpp", "display_logic.cpp"], []),
}

# lv_conf.h through -I. (LV_CONF_INCLUDE_SIMPLE); LVGL's headers through -Ilvgl and
# -Ilvgl/include, as its own CMake build does; -I.. for the control program's
# constants.h and third_party/httplib.h
INCLUDES = ["-DLV_CONF_INCLUDE_SIMPLE", "-I.", "-I..", "-I" + LVGL, "-I" + os.path.join(LVGL, "include"),
            "-I" + os.path.join(LVGL, "src", "libs", "thorvg")]
C_FLAGS = ["-O2", "-Wall", "-std=gnu17", *INCLUDES]
CXX_FLAGS = ["-O2", "-Wall", "-std=c++20", *INCLUDES]


def obj_path(src):
    # lvgl/src/core/lv_obj.c -> build/lvgl/src/core/lv_obj.o (LVGL repeats some file names)
    return os.path.join(BUILD, os.path.splitext(src)[0] + ".o")


def stale(src, newest_dependency):
    obj = obj_path(src)
    if not os.path.exists(obj):
        return True
    built = os.path.getmtime(obj)
    return built < os.path.getmtime(src) or built < newest_dependency


def compile_one(src):
    os.makedirs(os.path.dirname(obj_path(src)), exist_ok=True)
    compiler, flags = ("gcc", C_FLAGS) if src.endswith(".c") else ("g++", CXX_FLAGS)
    result = subprocess.run([compiler, *flags, "-c", src, "-o", obj_path(src)],
                            capture_output=True, text=True)
    return src, result


def compile_all(sources):
    # all cores; stop at the first failure and show its errors
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for src, result in pool.map(compile_one, sources):
            if result.returncode != 0:
                sys.exit(f"{src}:\n{result.stderr}")
            if result.stderr:
                print(result.stderr, end="")


if not os.path.exists(os.path.join(LVGL, "lvgl.h")):
    sys.exit("lvgl is missing: run  git submodule update --init pi/display/lvgl  (on the Ubuntu side)")

preview = "--preview" in sys.argv
program = "display_preview" if preview else "thermostat_display"
app_sources, libraries = TARGETS[program]
conf_time = os.path.getmtime("lv_conf.h")

# ---- LVGL -> build/liblvgl.a
lvgl_sources = sorted(glob.glob(os.path.join(LVGL, "src", "**", "*.c"), recursive=True) +
                      glob.glob(os.path.join(LVGL, "src", "**", "*.cpp"), recursive=True))
lvgl_stale = [s for s in lvgl_sources if stale(s, conf_time)]
library = os.path.join(BUILD, "liblvgl.a")
if lvgl_stale:
    print(f"compiling {len(lvgl_stale)} LVGL files (first build: ~10+ minutes on a Pi 3)")
    compile_all(lvgl_stale)
if lvgl_stale or not os.path.exists(library):
    if os.path.exists(library):
        os.remove(library)
    subprocess.run(["ar", "rcs", library, *[obj_path(s) for s in lvgl_sources]], check=True)

# ---- our code: rebuilt when it, a header it may use, or lv_conf.h changes
headers = glob.glob("*.h") + glob.glob("../*.h") + glob.glob("../third_party/*.h")
newest = max([conf_time] + [os.path.getmtime(h) for h in headers])
app_stale = [s for s in app_sources if stale(s, newest)]
for src in app_stale:
    print("compiling", src)
compile_all(app_stale)

# -lm: LVGL's maths; -pthread: the API worker thread
subprocess.run(["g++", "-pthread", "-o", program, *[obj_path(s) for s in app_sources], library, "-lm",
                *libraries], check=True)

if preview:
    subprocess.run(["./" + program], check=True)
elif "--no-run" not in sys.argv:
    subprocess.run(["./" + program], check=True)
