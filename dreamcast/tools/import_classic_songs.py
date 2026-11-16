#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare songs for the Dreamcast library from Ogg stems or a mounted PS2 disc."""

import argparse
from contextlib import ExitStack, contextmanager
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unicodedata
import wave


def child_name(value):
    if (not value or value in {".", ".."} or value.startswith(".") or
            any(char in value for char in "/\\:\x00\r\n")):
        raise ValueError(f"Invalid library directory name: {value!r}")
    return value


def song_id(title):
    ascii_title = unicodedata.normalize("NFKD", title).encode("ascii", "ignore").decode()
    name = re.sub(r"[^a-z0-9]+", "-", ascii_title.lower()).strip("-")
    return child_name(name or title)


def write_song_info(folder, title, artist):
    if not title.strip() or any(char in title + artist for char in "\x00\r\n"):
        raise ValueError("Provide a title and artist that fit on one line")
    (folder / "song.ini").write_text(f"[song]\nname = {title}\nartist = {artist}\n", encoding="utf-8")


@contextmanager
def destination_directory(destination):
    if os.path.lexists(destination):
        raise FileExistsError(f"Song already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".fof-import-", dir=destination.parent) as temp:
        song = Path(temp) / "song"
        song.mkdir()
        yield song
        if os.path.lexists(destination):
            raise FileExistsError(f"Song already exists: {destination}")
        song.rename(destination)


def required_bytes(stream, size):
    result = stream.read(size)
    if len(result) != size:
        raise ValueError("Unexpected end of file")
    return result


def ogg_rate(path):
    # Each chained link or multiplexed stream begins with a BOS page.
    # The Dreamcast Vorbis playback expects a single logical stream.
    streams = 0
    with path.open("rb") as source:
        length = os.fstat(source.fileno()).st_size
        while source.tell() < length:
            header = required_bytes(source, 27)
            if header[:5] != b"OggS\0":
                raise ValueError(f"Invalid Ogg page: {path}")
            streams += bool(header[5] & 2)
            if streams > 1:
                raise ValueError(f"Use one logical Vorbis stream per file: {path}")
            source.seek(sum(required_bytes(source, header[26])), os.SEEK_CUR)
            if source.tell() > length:
                raise ValueError(f"Truncated Ogg file: {path}")
    if streams != 1:
        raise ValueError(f"Missing Ogg logical stream: {path}")

    report = subprocess.check_output(
        ["ffprobe", "-v", "error", "-show_entries",
         "stream=codec_name,channels,sample_rate,duration", "-of", "json", str(path)],
        text=True,
    )
    audio, = json.loads(report)["streams"]
    if audio["codec_name"] != "vorbis" or audio["channels"] not in (1, 2):
        raise ValueError(f"Use mono or stereo Ogg Vorbis: {path}")
    rate = int(audio["sample_rate"])
    duration = float(audio["duration"])
    if not 8000 <= rate <= 48000 or not math.isfinite(duration) or not 0 < duration <= 3600:
        raise ValueError(f"Audio must be 8–48 kHz and at most one hour: {path}")
    return rate


def create_blank_midi(path):
    # A blank chart in the editor has the classic default of 122 BPM.
    track = b"\x00\xff\x51\x03" + (60000000 // 122).to_bytes(3, "big") + b"\x00\xff\x2f\x00"
    path.write_bytes(
        b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) +
        b"MTrk" + struct.pack(">I", len(track)) + track
    )


def import_ogg(args):
    sources = {"guitar.ogg": args.guitar, "song.ogg": args.background}
    rates = {ogg_rate(source) for source in sources.values() if source}
    if len(rates) != 1:
        raise ValueError("The instrument and backing tracks must have the same sample rate")
    if args.rhythm:
        if ogg_rate(args.rhythm) not in rates:
            raise ValueError("The rhythm track must have the same sample rate as the instrument")
        sources["rhythm.ogg"] = args.rhythm
    if args.notes:
        with args.notes.open("rb") as source:
            if source.read(4) != b"MThd":
                raise ValueError(f"Invalid chart MIDI header: {args.notes}")
    destination = args.library / (child_name(args.id) if args.id else song_id(args.name))
    with destination_directory(destination) as song:
        write_song_info(song, args.name, args.artist)
        for filename, source in sources.items():
            if source:
                shutil.copyfile(source, song / filename)
        if args.notes:
            shutil.copyfile(args.notes, song / "notes.mid")
        else:
            create_blank_midi(song / "notes.mid")
    print(destination)


class ArkArchive:
    """Read the index layout used by Editor.ArkFile in Frets on Fire 1.3.110."""

    def __init__(self, header_path, ark_path):
        self.path = ark_path
        archive_size = ark_path.stat().st_size
        with header_path.open("rb") as header:
            header_size = os.fstat(header.fileno()).st_size
            _, _, _, _, names_size = struct.unpack("<5I", required_bytes(header, 20))
            if names_size + 8 > header_size - header.tell():
                raise ValueError("Invalid HDR name table size")
            names = required_bytes(header, names_size)
            count, = struct.unpack("<I", required_bytes(header, 4))
            if count > (header_size - header.tell() - 4) // 4:
                raise ValueError("Truncated HDR name index")
            offsets = struct.iter_unpack("<I", required_bytes(header, count * 4))
            strings = []
            for (offset,) in offsets:
                end = names.find(b"\0", offset)
                if offset >= names_size or end < 0:
                    raise ValueError("Invalid HDR name offset")
                strings.append(names[offset:end])
            count, = struct.unpack("<I", required_bytes(header, 4))
            if count > (header_size - header.tell()) // 20:
                raise ValueError("Truncated HDR file index")
            self.files = {}
            for _ in range(count):
                offset, filename, directory, length, _ = struct.unpack("<5I", required_bytes(header, 20))
                if filename >= len(strings) or directory >= len(strings) or offset + length > archive_size:
                    raise ValueError("Invalid HDR archive entry")
                name = b"/".join((strings[directory], strings[filename])).lower()
                previous = self.files.setdefault(name, (offset, length))
                if previous != (offset, length):
                    raise ValueError(f"Duplicate HDR archive entry: {name!r}")

    @contextmanager
    def open(self, name):
        offset, length = self.files[name]
        with self.path.open("rb") as source:
            source.seek(offset)
            yield source, length

    def extract(self, name, destination):
        with self.open(name) as (source, length), destination.open("wb") as output:
            while length:
                data = required_bytes(source, min(length, 65536))
                output.write(data)
                length -= len(data)


# XA/VAG coefficients and the asymmetric sample rounding match the Linux
# 1.3.110 GHImporter decoder. Histories belong to the interleaved channel.
VGS_COEFFICIENTS = ((0.0, 0.0), (60 / 64, 0.0), (115 / 64, -52 / 64), (98 / 64, -55 / 64),
                    (122 / 64, -60 / 64), (0.0, 0.0), (0.0, 0.0), (0.0, 0.0))


class VgsChannel:
    def __init__(self):
        self.previous = self.older = 0.0

    def decode(self, block):
        predictor, shift = block[0] >> 4, block[0] & 15
        if predictor >= len(VGS_COEFFICIENTS):
            raise ValueError("Invalid VGS predictor")
        first, second = VGS_COEFFICIENTS[predictor]
        samples = []
        for byte in block[2:]:
            for nibble in (byte & 15, byte >> 4):
                sample = ((nibble if nibble < 8 else nibble - 16) << 12) >> shift
                result = sample + (self.previous * first + self.older * second)
                self.older, self.previous = self.previous, result
                samples.append(max(-32768, min(32767, int(result + 0.5))))
        return struct.pack("<28h", *samples)


def split_vgs(source, length, pcm):
    header_bytes = 128
    if length < header_bytes:
        raise ValueError("Truncated VGS header")
    header = required_bytes(source, header_bytes)
    if header[:4] != b"VgS!":
        raise ValueError("Missing VGS header")
    rates = []
    for rate, blocks in struct.iter_unpack("<II", header[8:]):
        if not rate or not blocks:
            break
        if not 1000 <= rate <= 192000:
            raise ValueError(f"Invalid VGS sample rate: {rate}")
        rates.append(rate)
    if len(rates) not in (4, 5, 6):
        raise ValueError(f"The classic importer expects 4, 5 or 6 VGS channels, got {len(rates)}")
    decoders = [VgsChannel() for _ in rates]
    remaining = length - header_bytes
    while remaining:
        if remaining < 16:
            raise ValueError("Partial VGS data block")
        block = required_bytes(source, 16)
        remaining -= 16
        flags = block[1]
        if flags == 7 or flags >= 128:
            break
        channel = flags % len(VGS_COEFFICIENTS)
        if channel < len(rates):
            pcm[channel].write(decoders[channel].decode(block))
    if any(not file.tell() for file in pcm[:len(rates)]):
        raise ValueError("VGS has an empty audio channel")
    return rates


def stereo_wave(left, right, output_path, rate):
    left.seek(0)
    right.seek(0)
    with wave.open(str(output_path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(rate)
        while True:
            left_chunk = left.read(16384)
            right_chunk = left_chunk if left is right else right.read(16384)
            size = min(len(left_chunk), len(right_chunk))
            if not size:
                break
            frames = bytearray(size * 2)
            frames[0::4] = left_chunk[:size:2]
            frames[1::4] = left_chunk[1:size:2]
            frames[2::4] = right_chunk[:size:2]
            frames[3::4] = right_chunk[1:size:2]
            output.writeframesraw(frames)


def encode_vorbis(source, target):
    if shutil.which("oggenc"):
        command = ["oggenc", "--quiet", "--resample", "44100", "-q", "6", "-o", str(target), str(source)]
    else:
        command = ["ffmpeg", "-nostdin", "-v", "error", "-i", str(source), "-ar", "44100",
                   "-c:a", "libvorbis", "-q:a", "6", str(target)]
    subprocess.run(command, check=True)


def write_vgs_tracks(source, length, folder):
    with tempfile.TemporaryDirectory(prefix="fof-vgs-") as temp:
        scratch = Path(temp)
        with ExitStack() as files:
            pcm = [files.enter_context((scratch / f"{i}.pcm").open("w+b")) for i in range(6)]
            rates = split_vgs(source, length, pcm)
            pairs = [(0, 1, "song"), (2, 3, "guitar")]
            if len(rates) >= 5:
                pairs.append((4, 5 if len(rates) == 6 else 4, "rhythm"))
            wave_path = scratch / "track.wav"
            for left, right, name in pairs:
                stereo_wave(pcm[left], pcm[right], wave_path, rates[left])
                encode_vorbis(wave_path, folder / f"{name}.ogg")


def gh_metadata(path):
    library = ""
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if not line.strip():
            continue
        fields = [field.strip() for field in line.split(";")]
        if len(fields) == 2 and fields[0] == "$library":
            library = child_name(fields[1])
        elif len(fields) == 3:
            name, title, artist = fields
            yield child_name(name), library, title, artist
        else:
            raise ValueError(f"Invalid classic track metadata: {line!r}")


def import_gh(args):
    gen = args.disc / "gen"
    # Mounted ISO9660 can expose either case across desktop operating systems.
    disk_entries = {entry.name.lower(): entry for entry in args.disc.iterdir()}
    gen = disk_entries.get("gen", gen)
    archive_entries = {entry.name.lower(): entry for entry in gen.iterdir()}
    archive = ArkArchive(archive_entries.get("main.hdr", gen / "main.hdr"),
                         archive_entries.get("main_0.ark", gen / "main_0.ark"))
    imported = 0
    found = 0
    wanted = {name.lower() for name in args.song}
    for name, library, title, artist in gh_metadata(args.classic_data / "ghmidimap.txt"):
        if wanted and name.lower() not in wanted:
            continue
        base = f"songs/{name}/{name}".lower().encode("ascii")
        midi = base + b".mid"
        vgs = base + b".vgs"
        if vgs not in archive.files:
            vgs = base + b"_sp.vgs"
        if vgs not in archive.files or midi not in archive.files:
            continue
        found += 1
        destination = args.library / library / name
        if os.path.lexists(destination):
            print(f"Kept existing song: {destination}")
            continue
        with destination_directory(destination) as song:
            write_song_info(song, title, artist)
            archive.extract(midi, song / "notes.mid")
            with archive.open(vgs) as (source, length):
                write_vgs_tracks(source, length, song)
        imported += 1
        print(destination)
    if not found:
        raise ValueError("The archive has no matching classic MIDI and VGS entries")
    print(f"Imported {imported} of {found} matching songs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="source", required=True)

    ogg = subparsers.add_parser("ogg", help="copy Ogg stems and prepare a new song")
    ogg.add_argument("--library", type=Path, required=True)
    ogg.add_argument("--name", required=True)
    ogg.add_argument("--artist", default="")
    ogg.add_argument("--id", help="directory name, using the title with hyphens by default")
    ogg.add_argument("--guitar", type=Path, required=True)
    ogg.add_argument("--background", type=Path)
    ogg.add_argument("--rhythm", type=Path)
    ogg.add_argument("--notes", type=Path, help="existing MIDI or a blank chart by default")
    ogg.set_defaults(run=import_ogg)

    gh = subparsers.add_parser("gh", help="extract songs from a mounted original Guitar Hero PS2 disc")
    gh.add_argument("--disc", type=Path, required=True)
    gh.add_argument("--classic-data", type=Path, required=True, help="the original 1.3.110 data directory")
    gh.add_argument("--library", type=Path, required=True)
    gh.add_argument("--song", action="append", default=[], help="restrict import to a song ID; repeatable")
    gh.set_defaults(run=import_gh)

    args = parser.parse_args()
    try:
        args.run(args)
    except (ValueError, OSError, subprocess.SubprocessError, KeyError) as error:
        parser.exit(1, f"Import error: {error}\n")


if __name__ == "__main__":
    main()
