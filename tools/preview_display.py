#!/usr/bin/env python3
"""Render the firmware's actual 128x64 OLED frames as PNGs and an animated GIF.

Requires a host C compiler and Pillow (python3 -m pip install Pillow).
Run from any directory: python3 tools/preview_display.py
The generated C harness calls GameDisplay_Render; no artwork is duplicated here.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT, FRAME_BYTES = 128, 64, 1024


@dataclass
class PreviewFrame:
    name: str
    label: str
    duration_ms: int
    values: dict[str, int | bool | str]
    key: bool = False


def scenarios() -> list[PreviewFrame]:
    frames: list[PreviewFrame] = []
    elapsed = 0

    def add(name: str, label: str, scene: str, duration: int = 150,
            key: bool = False, **changes: int | bool | str) -> None:
        nonlocal elapsed
        values: dict[str, int | bool | str] = {
            "scene": f"EXTERNAL_SCENE_{scene}",
            "now_ms": elapsed,
            "score": 25,
            "energy": 3,
            "lives": 3,
            "target": "EXTERNAL_COLOUR_RED",
            "target_age_ms": 1800,
            "target_period_ms": 4000,
            "effect": "EXTERNAL_EFFECT_NONE",
            "effect_age_ms": 1000,
            "zombie_hits": 2,
            "goal_hits": 6,
        }
        values.update(changes)
        frames.append(PreviewFrame(name, label, duration, values, key))
        elapsed += duration

    for i in range(8):
        add(f"standby-{i}", "STANDBY / double press to play", "STANDBY",
            key=i == 0, energy=0, score=0, zombie_hits=0)

    for i in range(19):
        age = i * 200
        add(f"zombie-{age}", f"ZOMBIE / approaches at {age / 1000:.1f}s", "BATTLE",
            200, key=age in (0, 1800, 3600), target_age_ms=age, energy=0)

    for i in range(5):
        add(f"charge-{i}", "CHARGE / +3 energy", "BATTLE", 100,
            key=i == 2, energy=3, effect="EXTERNAL_EFFECT_CHARGE",
            effect_age_ms=i * 100, effect_target="EXTERNAL_COLOUR_RED",
            effect_target_age_ms=1800, aim_locked=True, sensor_warning=True)

    for i in range(7):
        add(f"hit-{i}", "PEA SHOT / zombie hit", "BATTLE", 75,
            key=i in (1, 4), score=35, energy=1, zombie_hits=3,
            target="EXTERNAL_COLOUR_GREEN", target_age_ms=i * 75,
            effect="EXTERNAL_EFFECT_HIT", effect_age_ms=i * 75,
            effect_target="EXTERNAL_COLOUR_RED", effect_target_age_ms=2200)

    for i in range(16):
        age = i * 240
        add(f"human-{age}", "HUMAN / let the survivor pass", "BATTLE", 240,
            key=i == 8, target="EXTERNAL_COLOUR_GREEN", target_age_ms=age,
            score=35, energy=1, zombie_hits=3)

    for i in range(6):
        add(f"human-saved-{i}", "SURVIVOR SAVED / +5 score", "BATTLE", 90,
            key=i == 3, target="EXTERNAL_COLOUR_RED", target_age_ms=i * 90,
            score=40, energy=1, zombie_hits=3, effect="EXTERNAL_EFFECT_HUMAN_SAVED",
            effect_age_ms=i * 90, effect_target="EXTERNAL_COLOUR_GREEN",
            effect_target_age_ms=3900)

    for i in range(6):
        add(f"human-hit-{i}", "WRONG SHOT / survivor hurt", "BATTLE", 90,
            key=i == 3, target="EXTERNAL_COLOUR_RED", target_age_ms=i * 90,
            score=35, lives=2, energy=1, zombie_hits=3,
            effect="EXTERNAL_EFFECT_HUMAN_HIT", effect_age_ms=i * 90,
            effect_target="EXTERNAL_COLOUR_GREEN", effect_target_age_ms=2100)

    for i in range(12):
        seconds = 10 - (i * 7 // 11)
        add(f"attacked-{i}", f"ATTACKED / rescue in {seconds}s", "ATTACKED", 150,
            key=i in (0, 11), attacked=True, rescue_seconds=seconds, lives=2)

    for i in range(6):
        add(f"rescued-{i}", "RESCUED / back to the garden", "BATTLE", 90,
            key=i == 3, lives=2, effect="EXTERNAL_EFFECT_RESCUED",
            effect_age_ms=i * 90)

    for i in range(10):
        add(f"won-{i}", "VICTORY / six zombies stopped", "WON", 150,
            key=i == 3, score=115, zombie_hits=6)

    for i in range(8):
        add(f"lost-{i}", "DEFEAT / garden overrun", "LOST", 150,
            key=i == 3, lives=0)

    add("terminated", "TERMINATED / reset to restart", "TERMINATED", 1600,
        key=True, lives=0)
    return frames


def compile_and_render(frames: list[PreviewFrame], compiler: str) -> bytes:
    initializers = []
    for frame in frames:
        fields = []
        for field, value in frame.values.items():
            literal = ("true" if value else "false") if isinstance(value, bool) else str(value)
            fields.append(f".{field} = {literal}")
        # Text fields remain valid for renderers that use context-sensitive hints.
        fields.extend(['.mode = "BATTLE"', '.status = "GARDEN DEFENSE"',
                       '.hint = "1x CHARGE 2x MODE"'])
        initializers.append("    { " + ", ".join(fields) + " }")
    source = """#include "game_display.h"
#include <stdint.h>
#include <stdio.h>
static const ExternalDisplayFrame frames[] = {
""" + ",\n".join(initializers) + """
};
int main(void) {
    uint8_t pixels[1024];
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; ++i) {
        GameDisplay_Render(pixels, &frames[i]);
        if (fwrite(pixels, 1, sizeof pixels, stdout) != sizeof pixels) return 1;
    }
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="ee2028-oled-preview-") as temporary:
        directory = Path(temporary)
        harness = directory / "render_frames.c"
        executable = directory / "render_frames"
        harness.write_text(source)
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-O2", "-I", str(ROOT / "Core/Inc"), str(harness),
                   str(ROOT / "Core/Src/game_display.c"), "-o", str(executable)]
        subprocess.run(command, check=True)
        rendered = subprocess.check_output([str(executable)])
    if len(rendered) != len(frames) * FRAME_BYTES:
        raise RuntimeError("Renderer returned an incomplete framebuffer stream")
    return rendered


def write_images(frames: list[PreviewFrame], data: bytes, destination: Path, scale: int) -> None:
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as error:
        raise SystemExit("Install the preview dependency: python3 -m pip install Pillow") from error

    destination.mkdir(parents=True, exist_ok=True)
    native_dir = destination / "native"
    native_dir.mkdir(exist_ok=True)
    pictures = []
    keyframes = []
    for index, frame in enumerate(frames):
        raw = data[index * FRAME_BYTES:(index + 1) * FRAME_BYTES]
        pixels = bytes(255 if raw[(y // 8) * WIDTH + x] & (1 << (y % 8)) else 0
                       for y in range(HEIGHT) for x in range(WIDTH))
        native = Image.frombytes("L", (WIDTH, HEIGHT), pixels)
        enlarged = native.resize((WIDTH * scale, HEIGHT * scale), Image.Resampling.NEAREST)
        pictures.append(enlarged)
        if frame.key:
            native.save(native_dir / f"{frame.name}.png")
            enlarged.save(destination / f"{frame.name}.png")
            keyframes.append((frame, enlarged))

    pictures[0].save(destination / "garden-game.gif", save_all=True,
                     append_images=pictures[1:], loop=0,
                     duration=[frame.duration_ms for frame in frames],
                     disposal=2, optimize=True)

    columns = 4
    margin, title_height, label_height = 20, 56, 36
    tile_width, tile_height = WIDTH * scale, HEIGHT * scale + label_height
    rows = (len(keyframes) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * (tile_width + margin) + margin,
                             rows * (tile_height + margin) + margin + title_height), "#101919")
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=16)
    title_font = ImageFont.load_default(size=22)
    draw.text((margin, 12), "Garden Guard - actual STM32 OLED renderer (128 x 64)",
              fill="#e8fff1", font=title_font)
    for index, (frame, picture) in enumerate(keyframes):
        x = margin + (index % columns) * (tile_width + margin)
        y = title_height + margin + (index // columns) * (tile_height + margin)
        sheet.paste(picture.convert("RGB"), (x, y))
        draw.text((x, y + HEIGHT * scale + 8), frame.label, fill="#bce2cf", font=font)
    sheet.save(destination / "contact-sheet.png")
    (destination / "frames.json").write_text(json.dumps([
        {"name": frame.name, "label": frame.label, "duration_ms": frame.duration_ms,
         "key": frame.key, "frame": frame.values} for frame in frames
    ], indent=2) + "\n")
    print(f"Rendered {len(frames)} actual C frames; {len(keyframes)} scene PNGs.")
    print(f"Animation: {destination / 'garden-game.gif'}")
    print(f"Contact sheet: {destination / 'contact-sheet.png'}")
    print(f"Native 128x64 frames: {native_dir}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "output/game-preview")
    parser.add_argument("--scale", type=int, default=4, choices=range(1, 9))
    parser.add_argument("--cc", default=shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("A host C compiler is required (clang or cc)")
    frames = scenarios()
    data = compile_and_render(frames, args.cc)
    write_images(frames, data, args.output.resolve(), args.scale)


if __name__ == "__main__":
    main()
