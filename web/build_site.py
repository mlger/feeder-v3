#!/usr/bin/env python3
"""Assemble the GitHub Pages site.

Copies the static pages, the firmware images with their web-flasher
manifests, and the WebAssembly demo into one output directory.

  build_site.py --out _site --version 0.2.0+abc1234 \\
      --hmi-dir hmi/.pio/build/hmi --node-bin node/.pio/build/nodemcuv2/firmware.bin \\
      --demo-dir build/web --releases-dir releases --release-tag v0.9.0

Earlier releases (one folder per tag, as downloaded with `gh release download`)
are published too, so the install page can offer any version. A release has
the controller app image and the feeder image; older ones lack the bootloader
and partition table, which are taken from this build (the partition layout has
not changed). Newer releases also carry a complete controller image.
"""
import argparse
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Flash offsets of the controller images; they follow hmi/partitions.csv.
HMI_PARTS = [
    ("bootloader.bin", 0x0),
    ("partitions.bin", 0x8000),
    ("ota_data_initial.bin", 0xF000),
    ("firmware.bin", 0x20000),
]


def version_key(tag):
    return [int(x) for x in re.findall(r"\d+", tag)]


def write_manifest(folder, name, title, version, chip, parts):
    manifest = {
        "name": title,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": chip, "parts": parts}],
    }
    with open(os.path.join(folder, name), "w") as f:
        json.dump(manifest, f, indent=2)


def find(folder, prefix):
    for name in sorted(os.listdir(folder)):
        if name.startswith(prefix) and name.endswith(".bin"):
            return os.path.join(folder, name)
    return None


def add_release(fw, src_dir, tag, hmi_dir):
    """Publish one earlier release under firmware/<tag>/. None if it has no usable files."""
    out = os.path.join(fw, tag)
    os.makedirs(out, exist_ok=True)
    entry = {"id": tag, "label": tag, "note": "", "controller": None, "feeder": None}
    version = tag.lstrip("v")

    # "aio-feeder-controller-full-" first: it is the complete image.
    full = find(src_dir, "aio-feeder-controller-full-")
    app = find(src_dir, "aio-feeder-controller-v")
    if full:
        shutil.copy(full, os.path.join(out, "controller-full.bin"))
        write_manifest(out, "manifest-hmi.json", "AIO-Feeder controller", version, "ESP32-S3",
                       [{"path": "controller-full.bin", "offset": 0}])
        entry["controller"] = "firmware/{}/manifest-hmi.json".format(tag)
    elif app and hmi_dir:
        shutil.copy(app, os.path.join(out, "controller-app.bin"))
        # The bootloader and partition table are shared with the current build.
        parts = [{"path": "../hmi-" + n, "offset": o} for n, o in HMI_PARTS[:3]]
        parts.append({"path": "controller-app.bin", "offset": HMI_PARTS[3][1]})
        write_manifest(out, "manifest-hmi.json", "AIO-Feeder controller", version, "ESP32-S3", parts)
        entry["controller"] = "firmware/{}/manifest-hmi.json".format(tag)
        entry["note"] = "The controller is installed with the bootloader and partition table of the latest build."

    node = find(src_dir, "aio-feeder-feeder-v")
    if node:
        shutil.copy(node, os.path.join(out, "feeder.bin"))
        write_manifest(out, "manifest-node.json", "AIO-Feeder feeder", version, "ESP8266",
                       [{"path": "feeder.bin", "offset": 0}])
        entry["feeder"] = "firmware/{}/manifest-node.json".format(tag)

    if not entry["controller"] and not entry["feeder"]:
        shutil.rmtree(out)
        return None
    return entry


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--hmi-dir")
    ap.add_argument("--node-bin")
    ap.add_argument("--demo-dir")
    ap.add_argument("--releases-dir", help="folders named by tag, with the files of each release")
    ap.add_argument("--release-tag", help="this build is being released as this tag")
    args = ap.parse_args()

    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    shutil.copytree(HERE, args.out, ignore=shutil.ignore_patterns("build_site.py", "__pycache__"))
    fw = os.path.join(args.out, "firmware")
    os.makedirs(fw, exist_ok=True)

    latest = {"id": "latest", "label": "Latest build " + args.version, "note": "", "controller": None, "feeder": None}
    if args.release_tag:
        latest["id"] = args.release_tag
        latest["label"] = "{} (this release)".format(args.release_tag)

    if args.hmi_dir:
        parts = []
        for name, offset in HMI_PARTS:
            src = os.path.join(args.hmi_dir, name)
            if not os.path.isfile(src):
                sys.exit("missing controller image: " + src)
            shutil.copy(src, os.path.join(fw, "hmi-" + name))
            parts.append({"path": "hmi-" + name, "offset": offset})
        write_manifest(fw, "manifest-hmi.json", "AIO-Feeder controller", args.version, "ESP32-S3", parts)
        latest["controller"] = "firmware/manifest-hmi.json"

    if args.node_bin:
        if not os.path.isfile(args.node_bin):
            sys.exit("missing feeder image: " + args.node_bin)
        shutil.copy(args.node_bin, os.path.join(fw, "node-firmware.bin"))
        write_manifest(fw, "manifest-node.json", "AIO-Feeder feeder", args.version, "ESP8266",
                       [{"path": "node-firmware.bin", "offset": 0}])
        latest["feeder"] = "firmware/manifest-node.json"

    versions = [latest]
    if args.releases_dir and os.path.isdir(args.releases_dir):
        for tag in sorted(os.listdir(args.releases_dir), key=version_key, reverse=True):
            if tag == args.release_tag:
                continue  # this build is that release
            entry = add_release(fw, os.path.join(args.releases_dir, tag), tag, args.hmi_dir)
            if entry:
                versions.append(entry)
    with open(os.path.join(fw, "versions.json"), "w") as f:
        json.dump({"versions": versions}, f, indent=2)

    if args.demo_dir:
        for name in ("feeder_demo.js", "feeder_demo.wasm"):
            src = os.path.join(args.demo_dir, name)
            if not os.path.isfile(src):
                sys.exit("missing demo file: " + src)
            shutil.copy(src, os.path.join(args.out, "demo", name))

    # Tell GitHub Pages to serve the files as they are.
    open(os.path.join(args.out, ".nojekyll"), "w").close()


if __name__ == "__main__":
    main()
