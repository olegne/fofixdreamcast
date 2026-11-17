#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read classic song.ini scores and pack a read-only Dreamcast score catalog."""

import argparse
from configparser import RawConfigParser, Error as ConfigError
from hashlib import sha1
from io import BytesIO
from pathlib import Path
import struct
import sys
import unicodedata


MAX_CATALOG = 512 * 1024
MAX_NAME = 20
MAX_SCORES = 5


class Index(int):
    """An object reference in a Cerealizer stream."""


def score_lists(data):
    """Decode the dictionary of score lists without instantiating Cerealizer classes."""
    stream = BytesIO(data)

    def count(limit):
        field = stream.readline(12)
        if not field.endswith(b"\n") or not field[:-1].isdigit():
            raise ValueError("invalid decimal field")
        number = int(field)
        if number > limit:
            raise ValueError("decimal field out of bounds")
        return number

    def reference():
        code = stream.read(1)
        if code in (b"i", b"l"):
            return count(0xffffffff)
        if code == b"r":
            return Index(count(len(objects) - 1))
        if code in (b"s", b"u"):
            size = count(4096)
            value = stream.read(size)
            if len(value) != size:
                raise ValueError("truncated string")
            return value
        raise ValueError("unsupported reference")

    if len(data) > 32 * 1024 or stream.readline(9) != b"cereal1\n":
        raise ValueError("invalid Cerealizer header")
    objects = [None] * count(1 + 4 + 4 * MAX_SCORES)
    for index in range(len(objects)):
        kind = stream.readline(8)
        if kind == b"tuple\n":
            if count(4) != 4:
                raise ValueError("invalid score record")
            objects[index] = (kind, tuple(reference() for _ in range(4)))
        elif kind in (b"dict\n", b"list\n"):
            objects[index] = (kind, [])
        else:
            raise ValueError("unexpected Cerealizer object")

    for kind, contents in objects:
        if kind == b"dict\n":
            contents.extend((reference(), reference()) for _ in range(count(4)))
        elif kind == b"list\n":
            contents.extend(reference() for _ in range(count(MAX_SCORES)))

    def resolve(index, kind):
        if not isinstance(index, Index) or objects[index][0] != kind:
            raise ValueError("wrong reference type")
        return objects[index][1]

    tables = resolve(reference(), b"dict\n")
    if stream.read(1):
        raise ValueError("trailing score data")

    result = {}
    for rows, difficulty in tables:
        if type(difficulty) is not int or not 0 <= difficulty < 4 or difficulty in result:
            raise ValueError("invalid difficulty")
        result[difficulty] = table = []
        for item in resolve(rows, b"list\n"):
            score, stars, name, digest = resolve(item, b"tuple\n")
            if (type(score) is not int or type(stars) is not int or stars > 5 or
                    not isinstance(name, bytes) or not isinstance(digest, bytes)):
                continue
            wanted = sha1(f"{difficulty}{score}{stars}".encode("ascii") + name).hexdigest().encode("ascii")
            if digest != wanted:
                continue
            try:
                decoded = name.decode("utf-8")
            except UnicodeDecodeError:
                decoded = name.decode("latin-1")
            decoded = unicodedata.normalize("NFC", decoded)
            if not all(ch.isprintable() for ch in decoded):
                continue
            table.append((score, stars, decoded[:MAX_NAME].encode("utf-8")))
        table.sort(key=lambda row: -row[0])
    return result


def pack_catalog(cd):
    entries = []
    size = 11
    for root in ("songs", "imports"):
        folder = cd / root
        for ini in sorted(folder.rglob("song.ini")):
            relative = ini.parent.relative_to(cd)
            if not (ini.parent / "notes.mid").is_file():
                continue
            try:
                config = RawConfigParser(strict=False)
                config.read_string(ini.read_text(encoding="latin-1"))
                hex_data = config.get("song", "scores", fallback="").strip()
                if not hex_data:
                    continue
                if len(hex_data) > 64 * 1024:
                    raise ValueError("score data too large")
                tables = score_lists(bytes.fromhex(hex_data))
            except (ConfigError, ValueError) as error:
                print(f"{relative}/song.ini: {error}", file=sys.stderr)
                continue
            if not any(tables.values()):
                continue
            chart = "/rd/tutorial/notes.mid" if relative.as_posix() == "songs/tutorial" else (
                f"/cd/{relative.as_posix()}/notes.mid")
            key = chart.encode("utf-8")
            if len(key) > 255:
                print(f"{relative}: score catalog path too long", file=sys.stderr)
                continue
            payload = bytearray(bytes([len(key)]) + key)
            for level in range(4):
                rows = tables.get(level, [])
                payload.append(len(rows))
                for score, stars, name in rows:
                    payload += struct.pack("<IBB", score, stars, len(name)) + name
            if size + len(payload) > MAX_CATALOG:
                raise ValueError("Dreamcast score catalog exceeds 512 KiB. Use a smaller song library.")
            size += len(payload)
            entries.append(payload)

    output = cd / "scores.dcs"
    if entries:
        output.write_bytes(b"FOF\x02" + struct.pack("<IBH", 0, 0, len(entries)) + b"".join(entries))
        print(f"Classic score tables imported for {len(entries)} song(s)")
    elif output.exists():
        output.unlink()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cd", type=Path)
    pack_catalog(parser.parse_args().cd)
