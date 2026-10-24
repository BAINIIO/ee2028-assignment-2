#!/usr/bin/env python3
"""Create a source ZIP and separate slides; never upload or submit anything."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def identifier(value):
    if not re.fullmatch(r"[A-Za-z0-9]+", value):
        raise argparse.ArgumentTypeError("Use letters and digits only")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", type=identifier, default="DRAFT")
    parser.add_argument("--members", type=identifier, nargs="+", default=[])
    args = parser.parse_args()
    if args.group != "DRAFT" and not args.members:
        parser.error("Supply --members when using an actual group number")
    label = "Assignment2_" + "_".join([args.group, *args.members])
    output = ROOT / "output/submission"
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"{label}_Code.zip"
    # Historical starter code is not part of the application and contains the
    # old printf/HAL_Delay examples. Keep it out of the submission archive.
    folders = ["Core", "Drivers", "tools", "tests", "docs", ".vscode"]
    files = ["Makefile", "README.md", ".gitignore", "STM32L4S5VITX_FLASH.ld",
             "STM32L4S5VITX_RAM.ld"]
    sources = [ROOT / name for name in files if (ROOT / name).is_file()]
    for folder in folders:
        sources.extend(path for path in (ROOT / folder).rglob("*")
                       if path.is_file() and "__pycache__" not in path.parts
                       and path.name != ".DS_Store" and path.suffix != ".pyc")
    for name in ("Garden_Guard_Player_Guide.pdf", "Garden_Guard_Code_Guidebook.pdf"):
        guide = ROOT / "output/pdf" / name
        if guide.is_file():
            sources.append(guide)
    for name in ("zombie-1800.png", "human-1920.png"):
        illustration = ROOT / "output/game-preview/native" / name
        if illustration.is_file():
            sources.append(illustration)
    # Keep the current verification record usable in an exported project,
    # without distributing flash backups or unrelated historical binaries.
    evidence = ROOT / "output/hardware-tests/20261024-153523-compliance"
    if evidence.is_dir():
        sources.extend(path for path in evidence.iterdir()
                       if path.is_file() and path.suffix in {".json", ".log"})
    checksums = []
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(set(sources)):
            relative = path.relative_to(ROOT)
            data = path.read_bytes()
            checksums.append(f"{hashlib.sha256(data).hexdigest()}  {relative.as_posix()}")
            bundle.write(path, Path("Assignment2") / relative)
        bundle.writestr("Assignment2/SHA256SUMS.txt", "\n".join(checksums) + "\n")
    print(f"Source archive: {archive}")
    presentation = ROOT / "output/pdf/Assignment2_DRAFT_Slides.pdf"
    if presentation.exists():
        slides = output / f"{label}_Slides.pdf"
        shutil.copy2(presentation, slides)
        print(f"Separate slides: {slides}")
    if args.group == "DRAFT":
        print("DRAFT names: fill group/matriculation details and verify hardware before submission.")


if __name__ == "__main__":
    main()
