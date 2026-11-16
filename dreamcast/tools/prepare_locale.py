# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack the Linux gettext catalogs and their international font for the port."""
from gettext import GNUTranslations
import struct
from zlib import compress

from PIL import Image, ImageDraw, ImageFont

LANGUAGES = ("brazilian_portuguese", "czech", "dutch", "finnish", "french", "galician", "german",
             "hebrew", "hungarian", "italian", "polish", "russian", "spanish", "swedish", "turkish")


def pack_texture(atlas, path, texture):
    texture(atlas, path)
    raw = path.read_bytes()
    path.with_suffix(".texz").write_bytes(raw[:8] + compress(raw[8:], level=9))
    path.unlink()


def locales(data, romdisk, texture):
    destination = romdisk / "lang"
    destination.mkdir()
    letters = set(map(chr, range(32, 127)))
    for index, name in enumerate(LANGUAGES, 1):
        with (data / "translations" / f"{name}.mo").open("rb") as source:
            catalog = GNUTranslations(source)._catalog
        messages = {key: value for key, value in catalog.items() if isinstance(key, str) and key and value}
        packed = bytearray()
        for key, value in sorted(messages.items()):
            packed.extend(f"{key}\0{value}\0".encode("utf-8"))
            letters.update(ch for ch in value if ch.isprintable())
        (destination / f"{index}.cat").write_bytes(packed)

    visual = romdisk / "theme"
    # Additional printable Latin characters can occur in artist names and custom songs.
    letters.update(chr(cp) for cp in range(0xa0, 0x100) if chr(cp).isprintable())
    font = ImageFont.truetype(str(data / "international.ttf"), 25)
    records = bytearray()
    alpha = Image.new("L", (512, 512))
    x = y = 0
    for glyph in sorted(letters):
        left, _, right, _ = font.getbbox(glyph, anchor="ls")
        left = min(0, left)
        advance = round(font.getlength(glyph))
        width = max(right, advance) - left + 4
        if x + width > alpha.width:
            x, y = 0, y + 36
        if y + 36 > alpha.height:
            raise ValueError("The international glyph atlas exceeds 512 by 512")
        cell = Image.new("L", (width, 36))
        ImageDraw.Draw(cell).text((2 - left, 26), glyph, font=font, fill=255, anchor="ls")
        alpha.paste(cell, (x, y))
        records.extend(struct.pack("<IHHBBh", ord(glyph), x, y, width, advance, left))
        x += width
    atlas = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
    atlas.putalpha(alpha)
    pack_texture(atlas, visual / "font-intl.tex", texture)
    (visual / "font-intl.map").write_bytes(records)

    title = ImageFont.truetype(str(data / "international.ttf"), 108)
    alpha = Image.new("L", (1024, 128))
    painter = ImageDraw.Draw(alpha)
    widths = bytearray()
    for index, glyph in enumerate("0123456789"):
        left, _, right, _ = title.getbbox(glyph, anchor="ls")
        left = min(0, left)
        width = max(right, round(title.getlength(glyph))) - left
        if width > 92:
            raise ValueError("International title glyph exceeds its cell")
        painter.text((96 * index + 2 - left, title.getmetrics()[0]), glyph,
                     font=title, fill=255, anchor="ls")
        widths.append(width)
    atlas = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
    atlas.putalpha(alpha)
    pack_texture(atlas, visual / "title-intl.tex", texture)
    (visual / "title-intl.map").write_bytes(widths)
