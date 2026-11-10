"""PlatformIO pre-build script.

LVGL ships ARM-only assembly sources (Helium, NEON). PlatformIO would try to
assemble them for the ESP32 and fail, so leave them out of the build.
"""
Import("env")  # noqa: F821 - provided by PlatformIO


def _skip(node):
    return None


env.AddBuildMiddleware(_skip, "*/lvgl/src/*.S")  # noqa: F821
