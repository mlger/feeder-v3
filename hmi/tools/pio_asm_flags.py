"""PlatformIO pre-build script.

LVGL ships ARM assembly sources (Helium, NEON). They compile to nothing on
other CPUs, but only if the preprocessor knows it is reading assembly: LVGL's
headers check __ASSEMBLY__, which GCC does not define by itself.
"""
Import("env")  # noqa: F821 - provided by PlatformIO

env.Append(ASPPFLAGS=["-D__ASSEMBLY__"])  # noqa: F821
