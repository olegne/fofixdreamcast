#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack optional cassette and library labels for a Dreamcast disc."""

import argparse
from pathlib import Path
import re

from PIL import Image

from prepare_assets import texture
from prepare_locale import pack_texture


def pack_label(source, destination):
    with Image.open(source) as original:
        image = original.convert("RGBA").resize((128, 128), Image.Resampling.LANCZOS)
    pack_texture(image, destination, texture)


def labels(stock, cd):
    built_in = {Path(song) / "label.png" for song in ("bangbang", "defy", "twibmpg")}
    for root in ("songs", "imports"):
        folder = cd / root
        for label in folder.rglob("label.png"):
            relative = label.relative_to(folder)
            original = stock / relative
            if (root == "songs" and relative in built_in and original.is_file() and
                    original.read_bytes() == label.read_bytes()):
                continue
            pack_label(label, label.with_suffix(".tex"))


def editor_labels(source, destination):
    files = sorted(path for path in source.iterdir()
                   if path.is_file() and path.suffix.lower() == ".png")
    if len(files) > 252:
        raise ValueError("LABEL_DIR supports up to 252 PNG files")
    names = set()
    if files:
        destination.mkdir(parents=True, exist_ok=True)
    for path in files:
        key = path.stem
        if not re.fullmatch(r"[A-Za-z0-9 _-]{1,48}", key) or not key.strip():
            raise ValueError(f"Unsupported cassette label filename: {path.name}")
        if key.casefold() in names:
            raise ValueError(f"Duplicate cassette label filename: {path.name}")
        names.add(key.casefold())
        pack_label(path, destination / path.with_suffix(".tex").name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path)
    parser.add_argument("cd", type=Path)
    parser.add_argument("--choices", help="directory of optional editor label PNGs")
    arguments = parser.parse_args()
    labels(arguments.stock, arguments.cd)
    if arguments.choices:
        editor_labels(Path(arguments.choices), arguments.cd / "labels")
