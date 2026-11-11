"""PlatformIO pre-build script: take the firmware version from /VERSION.

The repository-root VERSION file is the one place the version is written.
Both firmwares, the simulator and the web site read it, so they cannot
disagree. A release is made by pushing a tag "v<VERSION>".
"""
import os

Import("env")  # noqa: F821 - provided by PlatformIO

_root = os.path.dirname(env.subst("$PROJECT_DIR"))  # noqa: F821
with open(os.path.join(_root, "VERSION"), encoding="utf-8") as f:
    _version = f.read().strip()
_major, _minor = (int(x) for x in _version.split(".")[:2])

env.Append(CPPDEFINES=[  # noqa: F821
    ("APP_VERSION", '\\"%s\\"' % _version),
    ("FW_MAJOR", _major),
    ("FW_MINOR", _minor),
])
