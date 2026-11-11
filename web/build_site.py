#!/usr/bin/env python3
"""Assemble the GitHub Pages site.

Copies the static pages, the firmware images with their web-flasher
manifests, and the WebAssembly demo into one output directory.

  build_site.py --out _site --version 0.2.0+abc1234 \\
      --hmi-dir hmi/.pio/build/hmi --node-bin node/.pio/build/nodemcuv2/firmware.bin \\
      --demo-dir build/web
"""
import argparse
import json
import os
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--hmi-dir")
    ap.add_argument("--node-bin")
    ap.add_argument("--demo-dir")
    args = ap.parse_args()

    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    shutil.copytree(HERE, args.out, ignore=shutil.ignore_patterns("build_site.py", "__pycache__"))
    fw = os.path.join(args.out, "firmware")
    os.makedirs(fw, exist_ok=True)

    if args.hmi_dir:
        parts = []
        for name, offset in HMI_PARTS:
            src = os.path.join(args.hmi_dir, name)
            if not os.path.isfile(src):
                sys.exit("missing controller image: " + src)
            shutil.copy(src, os.path.join(fw, "hmi-" + name))
            parts.append({"path": "hmi-" + name, "offset": offset})
        manifest = {
            "name": "AIO-Feeder controller",
            "version": args.version,
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
        }
        with open(os.path.join(fw, "manifest-hmi.json"), "w") as f:
            json.dump(manifest, f, indent=2)

    if args.node_bin:
        if not os.path.isfile(args.node_bin):
            sys.exit("missing feeder image: " + args.node_bin)
        shutil.copy(args.node_bin, os.path.join(fw, "node-firmware.bin"))
        manifest = {
            "name": "AIO-Feeder feeder",
            "version": args.version,
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": "ESP8266", "parts": [{"path": "node-firmware.bin", "offset": 0}]}],
        }
        with open(os.path.join(fw, "manifest-node.json"), "w") as f:
            json.dump(manifest, f, indent=2)

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
