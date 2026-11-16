#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Convert the Frets on Fire 1.3.110 Linux artwork to Dreamcast ROM disk assets."""
import array
import math
from pathlib import Path
import shutil
import struct
import sys

from PIL import Image, ImageDraw, ImageFont
from collada import Collada
from collada.common import DaeBrokenRefError

from prepare_classic_sounds import sounds


TEXTURES = {
    "keyboard": ("keyboard.png", (512, 128)),
    "pose": ("pose.png", (256, 256)),
    "logo": ("logo.png", (512, 128)),
    "cassette-bg": ("cassette.png", (256, 256)),
    "editor-bg": ("editor.png", (512, 256)),
    "stage-lights-left": ("stage_lights1.png", (256, 128)),
    "stage-lights-right": ("stage_lights2.png", (256, 128)),
    "stage-drums": ("stage_drums.png", (256, 256)),
    "stage-bass": ("stage_bassdrum.png", (128, 128)),
    "stage-speakers": ("stage_speakers.png", (256, 256)),
    "stage-cones": ("stage_speaker_cones.png", (128, 128)),
    "stage-crowd-left": ("stage_audience1.png", (256, 256)),
    "stage-crowd-right": ("stage_audience2.png", (256, 256)),
    "stage-beam": ("stage_light.png", (256, 256)),
    "neck": ("neck.png", (256, 256)),
    "string": ("string.png", (32, 256)),
    "bar": ("bar.png", (256, 32)),
    "tail": ("note.png", (32, 256)),
    "glow": ("glow.png", (128, 128)),
    "ball-on": ("ball2.png", (32, 32)),
    "ball-off": ("ball1.png", (32, 32)),
    "star": ("star2.png", (128, 128)),
    "star-off": ("star1.png", (128, 128)),
    "mult2": ("2x.png", (128, 128)),
    "mult3": ("3x.png", (128, 128)),
    "mult4": ("4x.png", (128, 128)),
    "tutorial-pose": ("songs/tutorial/pose.png", (256, 256)),
    "tutorial-keyboard": ("songs/tutorial/keyboard.png", (512, 256)),
    "tutorial-escape": ("songs/tutorial/esc.png", (256, 256)),
}


def texture(image, path, solid=False):
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


def additive_light_texture(image, path):
    # These greyscale PNGs use GL_SRC_ALPHA, GL_ONE. Preserving the product
    # before quantization retains the original beam intensity through filtering.
    channel = iter(image.tobytes())
    words = array.array("H")
    for r, g, b, a in zip(channel, channel, channel, channel):
        if r != g or r != b:
            raise ValueError(f"Expected a greyscale additive texture at {path}")
        intensity = (r * a + 127) // 255
        red = (31 * intensity + 127) // 255
        green = (63 * intensity + 127) // 255
        words.append((red << 11) | (green << 5) | red)
    if sys.byteorder != "little":
        words.byteswap()
    path.write_bytes(struct.pack("<4H", *image.size, 0, 0) + words.tobytes())


def font_atlas(source, out, name):
    font = ImageFont.truetype(str(source), 25)
    alpha = Image.new("L", (512, 256))
    widths = bytearray()
    for index, glyph in enumerate(map(chr, range(32, 127))):
        x = (index % 16) * 32
        y = (index // 16) * 36
        cell = Image.new("L", (32, 36))
        ImageDraw.Draw(cell).text((2, 26), glyph, font=font, fill=255, anchor="ls")
        alpha.paste(cell, (x, y))
        widths.append(round(font.getlength(glyph)))
    atlas = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
    atlas.putalpha(alpha)
    texture(atlas, out / (name + ".tex"))
    (out / (name + ".map")).write_bytes(widths)


def title_numbers(source, out):
    # Data.py opens title.ttf at 108 px. Only countdown and score use it.
    font = ImageFont.truetype(str(source), 108)
    baseline = font.getmetrics()[0]
    alpha = Image.new("L", (512, 128))
    painter = ImageDraw.Draw(alpha)
    widths = bytearray()
    for index, glyph in enumerate("0123456789"):
        left, _, right, _ = font.getbbox(glyph, anchor="ls")
        left = min(0, left)
        x = 48 * index + 2  # transparent padding keeps bilinear sampling within the glyph
        painter.text((x - left, baseline), glyph, font=font, fill=255, anchor="ls")
        widths.append(max(right, round(font.getlength(glyph))) - left)
    atlas = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
    atlas.putalpha(alpha)
    texture(atlas, out / "title-font.tex")
    (out / "title-font.map").write_bytes(widths)


def polygons(source, factors, orient=lambda p: p, scene_light=False):
    # key.dae lists an unused dangling #Scene before its real scene.
    document = Collada(str(source), ignore=[DaeBrokenRefError])
    if scene_light:
        lamp = next(node for node in document.scenes[0].nodes if any(node.objects("light")))
        direction = orient(lamp.matrix[:3, 3])
        length = math.sqrt(sum(v * v for v in direction))
        light = tuple(v / length for v in direction)
    else:
        light = (0.333, 0.667, -0.667)
    faces = []
    for node in document.scenes[0].nodes:
        if node.id not in factors:
            continue
        scale = math.sqrt(sum(v * v for v in node.matrix[:3, 0]))
        for geometry in node.objects("geometry"):
            for group in geometry.primitives():
                for polygon in group:
                    verts = []
                    for position, normal in zip(polygon.vertices, polygon.normals):
                        x, y, z = orient(position)
                        normal = orient(normal)
                        length = math.sqrt(sum(v * v for v in normal))
                        dot = sum(a * b for a, b in zip(normal, light)) / max(0.001, length)
                        # Mesh.render supplies a directional COLLADA light. OpenGL applies
                        # the inverse scale to unit normals when GL_NORMALIZE is disabled.
                        brightness = (0.2 + max(0.0, dot) / scale if scene_light else
                                      0.52 + 0.48 * max(0.0, min(1.0, dot)))
                        shade = factors[node.id] * brightness
                        verts.append((float(x), float(y), float(z), shade))
                    faces.append(verts)
    return faces


def store_mesh(faces, path):
    with path.open("wb") as packed:
        faces = [face for original in faces
                 for face in ([original] if len(original) <= 4 else
                              [[original[0], original[i], original[i + 1]]
                               for i in range(1, len(original) - 1)])]
        packed.write(struct.pack("<I", len(faces)))
        for face in faces:
            packed.write(struct.pack("<I", len(face)))
            if len(face) == 4:
                face = [face[i] for i in (0, 1, 3, 2)]
            for vertex in face:
                packed.write(struct.pack(f"<{len(vertex)}f", *vertex))


def preview(faces, project, size):
    image = Image.new("RGBA", size)
    draw = ImageDraw.Draw(image)
    projected = [[project(x, y, z) for x, y, z, _ in face] for face in faces]
    order = sorted(range(len(faces)), key=lambda i: -sum(v[2] for v in projected[i]) / len(projected[i]))
    for i in order:
        intensity = min(255, round(255 * sum(v[3] for v in faces[i]) / len(faces[i])))
        draw.polygon([(x, y) for x, y, _ in projected[i]],
                     fill=(intensity, intensity, intensity, 255))
    return image


def note_preview(x, y, z):
    # Project the original model at the hit line for small, distant notes.
    depth = (7 * (z + 3) - 3 * (y - 3)) / math.hypot(7, 3)
    up = (7 * (y - 3) + 3 * (z + 3)) / math.hypot(7, 3)
    return 64 - 896 * x / depth, 64 - 896 * (up / depth + 0.4), depth


def cassette_faces(document, node_name, sticker=False):
    nodes = document.scenes[0].nodes
    lamp = next(node for node in nodes if node.id.startswith("Lamp"))
    direction = lamp.matrix[:3, 3]
    direction = direction / math.hypot(*direction)
    light = next(child.light.color[0] for child in lamp.children if hasattr(child, "light"))
    node = next(node for node in nodes if node.id == node_name)
    scale = math.hypot(*node.matrix[:3, 0])
    faces = []
    for geometry in node.objects("geometry"):
        for group in geometry.primitives():
            for polygon in group:
                face = []
                for i, (point, normal) in enumerate(zip(polygon.vertices, polygon.normals)):
                    # Mesh.py uses directional diffuse plus OpenGL's global ambient.
                    # Normalize the bound direction, then apply the inverse model scale.
                    dot = sum(a * b for a, b in zip(normal, direction)) / math.hypot(*normal)
                    shade = 0.2 + light * max(0.0, dot) / scale
                    vertex = (*point, shade)
                    if sticker:
                        u, v = polygon.texcoords[0][i]
                        vertex += (u, 1.0 - v)  # SongChooser flips the label's V axis.
                    face.append(vertex)
                faces.append(face)
    return faces


def cassette_assets(data, out):
    cassette = Collada(str(data / "cassette.dae"))
    label = Collada(str(data / "label.dae"))
    store_mesh(cassette_faces(cassette, "Mesh_001"), out / "cassette-shell.mesh")
    store_mesh(cassette_faces(cassette, "Mesh"), out / "cassette-reels.mesh")
    store_mesh(cassette_faces(label, "Mesh_001", sticker=True), out / "cassette-sticker.mesh")
    for song in ("bangbang", "defy", "twibmpg"):
        label = data / "songs" / song / "label.png"
        with Image.open(label) as image:
            texture(image.convert("RGBA").resize((256, 128), Image.Resampling.LANCZOS),
                    out / ("label-" + song + ".tex"))


def main(data, out):
    for subdir in ("theme", "tutorial"):
        (out / subdir).mkdir(parents=True, exist_ok=True)
    visual = out / "theme"
    for name, (path, size) in TEXTURES.items():
        with Image.open(data / path) as src:
            scaled = src.convert("RGBA").resize(size, Image.Resampling.LANCZOS)
        destination = visual / (name + ".tex")
        if name in ("stage-beam", "glow"):
            additive_light_texture(scaled, destination)
        else:
            texture(scaled, destination)

    # The center 640x480 of stage.ini's 1.3x backdrop fills the viewport.
    with Image.open(data / "stage_background.png") as src:
        image = src.convert("RGBA")
    w, h = image.size
    background = image.crop((round(w / 2 - 320 / 1.3), round(h / 2 - 240 / 1.3),
                             round(w / 2 + 320 / 1.3), round(h / 2 + 240 / 1.3)))
    flat = Image.new("RGBA", background.size, (51, 0, 0, 255))
    flat.alpha_composite(background)
    texture(flat.resize((512, 512), Image.Resampling.LANCZOS), visual / "stage.tex", solid=True)

    font_atlas(data / "default.ttf", visual, "font")
    title_numbers(data / "title.ttf", visual)
    notes = polygons(data / "note.dae", {"Mesh_001": 1.0, "Mesh": 0.75, "Mesh_002": 0.25},
                     scene_light=True)
    hopo = polygons(data / "note.dae", {"Mesh_003": 1.0}, scene_light=True)
    # Compose Guitar.renderFrets' Y(+90), X(-90), Z(-90) rotations in that order.
    keys = polygons(data / "key.dae", {"Mesh": 1.0}, orient=lambda p: (p[0], p[2], -p[1]),
                    scene_light=True)
    store_mesh(notes, visual / "notes.mesh")
    store_mesh(hopo, visual / "hopo.mesh")
    store_mesh(keys, visual / "keys.mesh")
    texture(preview(notes, note_preview, (128, 128)), visual / "gem.tex")
    texture(preview(notes + hopo, note_preview, (128, 128)), visual / "tap-gem.tex")
    cassette_assets(data, visual)
    sounds(data, out)
    for name in ("notes.mid", "song.ogg", "guitar.ogg", "song.ini", "script.txt"):
        shutil.copyfile(data / "songs/tutorial" / name, out / "tutorial" / name)


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
