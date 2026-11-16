#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the original Circuito 01 backing track and its synchronized FoFiX chart."""

import array
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import wave


RATE = 32000
PPQN = 480
TEMPO_CHANGE = 24 * PPQN
FIRST_TEMPO = 500_000
SECOND_TEMPO = 428_571
BARS = 12

# Positions are eighth-note slots within a bar, followed by the held fret numbers.
PHRASES = (
    ((0, (0,)), (2, (1,)), (4, (2,)), (6, (3,))),
    ((0, (4,)), (3, (3,)), (5, (2,)), (7, (1,))),
    ((0, (0,)), (2, (0,)), (4, (2,)), (6, (4,))),
    ((0, (0, 4)), (3, (3,)), (4, (2,)), (6, (1,))),
    ((0, (0, 2)), (2, (3,)), (3, (2,)), (5, (4,)), (7, (1,))),
    ((0, (0, 4)), (2, (2,)), (4, (0,)), (5, (3,)), (6, (2,))),
)


def at_seconds(tick):
    before = min(tick, TEMPO_CHANGE)
    after = tick - before
    return (before * FIRST_TEMPO + after * SECOND_TEMPO) / (PPQN * 1_000_000)


def varint(value):
    result = [value & 127]
    value >>= 7
    while value:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(reversed(result))


def track(events):
    data = bytearray()
    previous = 0
    for tick, event in sorted(events, key=lambda item: item[0]):
        data += varint(tick - previous) + event
        previous = tick
    data += b"\0\xff\x2f\0"
    return b"MTrk" + struct.pack(">I", len(data)) + data


def chart_bytes(gems):
    tempos = [
        (0, b"\xff\x51\x03" + FIRST_TEMPO.to_bytes(3, "big")),
        (TEMPO_CHANGE, b"\xff\x51\x03" + SECOND_TEMPO.to_bytes(3, "big")),
    ]
    guitar = [(0, b"\xff\x03\x0bPART GUITAR")]
    for tick, frets in gems:
        for fret in frets:
            guitar.extend(((tick, bytes((0x90, 96 + fret, 100))),
                           (tick + PPQN // 3, bytes((0x80, 96 + fret, 0)))))
    return b"MThd" + struct.pack(">IHHH", 6, 1, 2, PPQN) + track(tempos) + track(guitar)


def render_audio(path, gems):
    frame_count = round((at_seconds(BARS * 4 * PPQN) + 0.7) * RATE)
    mix = [0.0] * frame_count
    rng = random.Random(4500)
    roots = (40, 40, 43, 38)

    def add(tick, seconds, voice):
        start = round(at_seconds(tick) * RATE)
        length = min(round(seconds * RATE), frame_count - start)
        for i in range(length):
            mix[start + i] += voice(i / RATE)

    def pluck(tick, midi, gain, seconds):
        hz = 440 * 2 ** ((midi - 69) / 12)

        def tone(t):
            phase = math.tau * hz * t
            partials = math.sin(phase) + 0.32 * math.sin(2 * phase) + 0.18 * math.sin(3 * phase)
            attack = min(1, t * 300)
            release = min(1, (seconds - t) * 55)
            return math.tanh(1.4 * partials) * attack * release * math.exp(-9 * t) * gain

        add(tick, seconds, tone)

    for bar in range(BARS):
        for slot in range(8):
            tick = (bar * 8 + slot) * PPQN // 2
            if slot % 2 == 0:
                pluck(tick, roots[bar % 4] + (7 if slot == 6 else 0), 0.21, 0.29)
            if slot in (0, 4):
                add(tick, 0.19, lambda t: 0.37 * math.sin(math.tau * (52 * t + 2 * (1 - math.exp(-44 * t)))) * math.exp(-24 * t))
            if slot in (2, 6):
                add(tick, 0.13, lambda t: 0.19 * rng.uniform(-1, 1) * math.exp(-28 * t))
            add(tick, 0.045, lambda t: 0.063 * rng.uniform(-1, 1) * math.exp(-95 * t))

    intervals = (12, 15, 17, 19, 22)
    for tick, frets in gems:
        bar = tick // (4 * PPQN)
        for fret in frets:
            pluck(tick, roots[bar % 4] + intervals[fret], 0.28 / math.sqrt(len(frets)), 0.24)

    pcm = array.array("h", (round(math.tanh(1.15 * s) * 30000) for s in mix))
    if sys.byteorder != "little":
        pcm.byteswap()
    with tempfile.TemporaryDirectory() as temporary:
        wav_path = Path(temporary) / "song.wav"
        with wave.open(str(wav_path), "wb") as wav:
            wav.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
            wav.writeframes(pcm.tobytes())
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                        "-i", str(wav_path), "-c:a", "libvorbis", "-q:a", "4", str(path)],
                       check=True)


def main(output):
    output.mkdir(parents=True, exist_ok=True)
    gems = [((bar * 8 + slot) * PPQN // 2, frets)
            for bar in range(1, BARS)
            for slot, frets in PHRASES[(bar - 1) % len(PHRASES)]]
    (output / "notes.mid").write_bytes(chart_bytes(gems))
    render_audio(output / "song.ogg", gems)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
