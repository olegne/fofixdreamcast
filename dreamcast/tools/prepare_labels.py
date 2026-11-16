#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack optional cassette and library labels for a Dreamcast disc."""

import argparse
from pathlib import Path

from PIL import Image

from prepare_assets import texture
from prepare_locale import pack_texture


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
            with Image.open(label) as source:
                image = source.convert("RGBA").resize((128, 128), Image.Resampling.LANCZOS)
            pack_texture(image, label.with_suffix(".tex"), texture)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path)
    parser.add_argument("cd", type=Path)
    arguments = parser.parse_args()
    labels(arguments.stock, arguments.cd)
