"""Prepare short AICA effects and preserve the original streamed audio."""

from pathlib import Path
import shutil
import subprocess


def sounds(source: Path, output: Path):
    effects = (
        "in", "out", "crunch1", "crunch2", "crunch3", "start",
        "fiba1", "fiba2", "fiba3", "fiba4", "fiba5", "fiba6",
    )
    result_effects = (
        "jurgen1", "jurgen2", "jurgen3", "jurgen4", "jurgen5",
        "myhero", "perfect1", "perfect2", "perfect3",
    )
    directory = output / "fx"
    directory.mkdir(parents=True, exist_ok=True)
    for effect in effects:
        subprocess.run(
            [
                "ffmpeg", "-nostdin", "-v", "error", "-y",
                "-i", str(source / (effect + ".ogg")),
                "-ar", "22050", "-c:a", "adpcm_yamaha",
                str(directory / (effect + ".wav")),
            ],
            check=True,
        )
    for effect in result_effects:
        name = effect + ".ogg"
        shutil.copyfile(source / name, directory / name)
    shutil.copyfile(source / "menu.ogg", output / "menu.ogg")
