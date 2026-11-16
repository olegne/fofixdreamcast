#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare the original FoFiX theme and tutorial for the PowerVR and AICA."""
import array
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont
from collada import Collada

import make_demo


THEME = "themes/MegaLight V4"
TEXTURES = {
    "main": (f"{THEME}/menu/mainbg.png", (512, 512), True),
    "maintext": (f"{THEME}/menu/maintext.png", (256, 512), False),
    "version": ("versiontag.png", (512, 32), False),
    "options": (f"{THEME}/menu/optionsbg.png", (512, 512), True),
    "stage": (f"{THEME}/backgrounds/default.png", (512, 512), True),
    "neck": ("necks/defaultneck.png", (256, 512), False),
    "neck-power": (f"{THEME}/board/overdriveneckovr.png", (128, 256), False),
    "strings": (f"{THEME}/board/center_lines.png", (256, 8), False),
    "bars": (f"{THEME}/board/side_bars.png", (8, 8), False),
    "measure": (f"{THEME}/board/bpm_measure.png", (128, 16), False),
    "beat": (f"{THEME}/board/bpm_beat.png", (128, 16), False),
    "half": (f"{THEME}/board/bpm_halfbeat.png", (128, 16), False),
    "frets": (f"{THEME}/frets/fretbuttons.png", (512, 256), False),
    "notes": (f"{THEME}/notes/notes.png", (256, 512), False),
    "flame": (f"{THEME}/flames/hitflames2.png", (128, 128), False),
    "glow": (f"{THEME}/flames/glow.png", (128, 128), False),
    "tail": ("tail1.png", (128, 32), False),
    "tail-end": ("tail2.png", (128, 32), False),
    "tail-held": ("bigtail1.png", (128, 32), False),
    "tail-held-end": ("bigtail2.png", (128, 32), False),
    "hud": (f"{THEME}/rockmeter/hud.png", (256, 64), False),
    "meter": (f"{THEME}/rockmeter/bar.png", (256, 512), False),
    "star": (f"{THEME}/rockmeter/star.png", (128, 128), False),
    "star-off": (f"{THEME}/rockmeter/graystar.png", (128, 128), False),
    "pause": (f"{THEME}/pause.png", (64, 64), False),
    "rock": (f"{THEME}/yourock.png", (256, 64), False),
    "fail": (f"{THEME}/youfailed.png", (256, 64), False),
    "tutorial-pose": ("tutorials/jurgenfof/pose.png", (128, 256), False),
    "tutorial-keyboard": ("tutorials/jurgenfof/keyboard.png", (256, 256), False),
    "tutorial-escape": ("tutorials/jurgenfof/esc.png", (128, 256), False),
}
for letter in "abcde":
    TEXTURES[f"gem-{letter}"] = (f"{THEME}/notes/notetex_{letter}.png", (128, 128), True)
    TEXTURES[f"power-{letter}"] = (f"{THEME}/notes/startex_{letter}.png", (128, 128), True)


def texture(image, path, solid):
    width, height = image.size
    if solid:
        pixels = iter(image.convert("RGB").tobytes())
        words = array.array("H", (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                                  for r, g, b in zip(pixels, pixels, pixels)))
    else:
        pixels = iter(image.convert("RGBA").tobytes())
        words = array.array("H", (((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
                                  for r, g, b, a in zip(pixels, pixels, pixels, pixels)))
    if sys.byteorder != "little":
        words.byteswap()
    path.write_bytes(struct.pack("<4H", width, height, int(not solid), 0) + words.tobytes())


def font_atlas(source, out, name):
    # The same bitmap is used for both drawing and measuring variable-width text.
    font = ImageFont.truetype(str(source), 25)
    alpha = Image.new("L", (512, 256))
    draw = ImageDraw.Draw(alpha)
    widths = bytearray()
    for index, glyph in enumerate(map(chr, range(32, 127))):
        x = (index % 16) * 32
        y = (index // 16) * 36
        draw.text((x + 2, y + 25), glyph, font=font, fill=255, anchor="ls")
        widths.append(round(font.getlength(glyph)))
    atlas = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
    atlas.putalpha(alpha)
    texture(atlas, out / (name + ".tex"), False)
    (out / (name + ".map")).write_bytes(widths)


def mesh(source, output):
    doc = Collada(str(source))
    for node_name, filename in (("Mesh", "notes.mesh"), ("Mesh_001", "hopo.mesh")):
        node = next(node for node in doc.scene.nodes if node.id == node_name)
        # BoundGeometry applies the Collada node transforms used by FoFiX's Mesh.render().
        faces = []
        for geometry in node.objects("geometry"):
            for polygon_set in geometry.primitives():
                for polygon in polygon_set:
                    verts = [(float(x), float(y), float(z), float(u), 1 - float(v))
                             for (x, y, z), (u, v) in zip(polygon.vertices, polygon.texcoords[0])]
                    if len(verts) == 4:
                        faces.append([verts[i] for i in (0, 1, 3, 2)])
                    elif len(verts) == 3:
                        faces.append(verts)
                    else:
                        faces.extend([verts[0], verts[i], verts[i + 1]]
                                     for i in range(1, len(verts) - 1))
        with (output / filename).open("wb") as packed:
            packed.write(struct.pack("<I", len(faces)))
            for face in faces:
                packed.write(struct.pack("<I", len(face)))
                for vertex in face:
                    packed.write(struct.pack("<5f", *vertex))


def sounds(data, out):
    sources = {name: data / f"{THEME}/sounds/{name}.ogg"
               for name in ("accept1", "back1", "select1", "select2", "select3", "start")}
    for name in ("staractivate", "stardeactivate", "starpowerready"):
        matches = (data / f"{THEME}/sounds/{name}.ogg", data / f"sounds/{name}.ogg")
        sources[name] = next((p for p in matches if p.exists()), data / "sounds/default.ogg")
    sources.update({name: data / f"sounds/{name}.ogg"
                    for name in ("failsound", "rocksound", "starding",
                                 *(f"guitscw{i}" for i in range(1, 7)))})
    # AICA sound-effect voices address at most 65,535 samples.
    for name, source in sources.items():
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                        "-i", str(source), "-ar", "32000", "-ac", "1", "-t", "2.047",
                        "-c:a", "pcm_s16le", str(out / (name + ".wav"))], check=True)


def main(data, out):
    for subdir in ("fx", "theme", "tutorial", "demo"):
        (out / subdir).mkdir(parents=True, exist_ok=True)
    for name, (path, size, solid) in TEXTURES.items():
        with Image.open(data / path) as src:
            scaled = src.convert("RGBA").resize(size, Image.Resampling.LANCZOS)
        texture(scaled, out / "theme" / (name + ".tex"), solid)
    font_atlas(data / f"{THEME}/fonts/default.ttf", out / "theme", "font")
    font_atlas(data / f"{THEME}/fonts/score.ttf", out / "theme", "score-font")
    mesh(data / f"{THEME}/notes/note.dae", out / "theme")
    sounds(data, out / "fx")
    for name in ("notes.mid", "song.ogg", "guitar.ogg", "song.ini", "script.txt"):
        shutil.copyfile(data / "tutorials/jurgenfof" / name, out / "tutorial" / name)
    make_demo.main(out / "demo")


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
