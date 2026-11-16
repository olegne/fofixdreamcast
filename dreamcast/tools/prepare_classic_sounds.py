"""Copy the original audio effects and menu music."""

from pathlib import Path
import shutil


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
    for effect in effects + result_effects:
        name = effect + ".ogg"
        shutil.copyfile(source / name, directory / name)
    shutil.copyfile(source / "menu.ogg", output / "menu.ogg")
