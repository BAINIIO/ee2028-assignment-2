#!/usr/bin/env python3
"""Explicit STM32 programming/debug commands; importing this file does no I/O."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
PLUGINS = Path("/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins")


def tool(name, pattern, variable):
    configured = os.environ.get(variable)
    if configured:
        executable = Path(configured).expanduser()
        if not executable.is_file():
            raise RuntimeError(f"{variable} does not name an executable")
        return str(executable)
    executable = shutil.which(name)
    if executable:
        return executable
    matches = sorted(PLUGINS.glob(pattern))
    if matches:
        return str(matches[-1])
    raise RuntimeError(f"Cannot find {name}; install STM32CubeIDE or set {variable}")


def programmer():
    return tool("STM32_Programmer_CLI", "*cubeprogrammer*/tools/bin/STM32_Programmer_CLI",
                "STM32_PROGRAMMER")


def inventory():
    # --list enumerates interfaces. It does not connect, halt, reset or flash.
    result = subprocess.run([programmer(), "--list"], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=False, timeout=30)
    return result.stdout, re.findall(r"ST-LINK\s+SN\s*:\s*([A-Za-z0-9]+)",
                                    result.stdout, flags=re.IGNORECASE)


def select_probe(serial):
    if serial:
        if not re.fullmatch(r"[A-Za-z0-9]+", serial):
            raise RuntimeError("Probe serial must contain only letters and digits")
        return serial
    _, probes = inventory()
    probes = sorted(set(probes))
    if len(probes) == 1:
        return probes[0]
    if not probes:
        raise RuntimeError("No ST-Link found. Connect a data cable to board port CN7. "
                           "Use 'detect' to inspect tool output.")
    raise RuntimeError("Multiple probes found; select one with --serial from 'detect'")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("detect", "flash", "debug", "gdb"))
    parser.add_argument("--serial", help="ST-Link serial; automatic if exactly one is found")
    args = parser.parse_args()
    try:
        if args.action == "detect":
            output, _ = inventory()
            print(output, end="")
            return 0
        elf = ROOT / "build/firmware/zombie_game.elf"
        if args.action == "gdb":
            subprocess.run(["sh", str(ROOT / "tools/arm-tool.sh"), "gdb",
                            "-x", str(ROOT / "tools/debug.gdb"), str(elf)],
                           cwd=ROOT, check=True)
            return 0
        serial = select_probe(args.serial)
        if args.action == "flash":
            if not elf.is_file():
                raise RuntimeError("Build the firmware first with 'make'. Flash uses that exact ELF.")
            print("Programming and verifying zombie_game.elf, then resetting the board.", flush=True)
            subprocess.run([programmer(), "-c", "port=SWD", f"sn={serial}",
                            "freq=4000", "mode=UR", "-w", str(elf), "-v", "-rst"],
                           cwd=ROOT, check=True)
            return 0
        executable = tool("openocd", "*openocd*/tools/bin/openocd", "STM32_OPENOCD")
        configured = os.environ.get("STM32_OPENOCD_SCRIPTS")
        scripts = ([Path(configured).expanduser()] if configured else
                   sorted(PLUGINS.glob("*debug.openocd*/resources/openocd/st_scripts")))
        if not scripts:
            raise RuntimeError("Set STM32_OPENOCD_SCRIPTS to ST's OpenOCD st_scripts directory")
        print("Starting debugger: board will halt/reset. Run board.py gdb in another terminal.",
              flush=True)
        subprocess.run([executable, "-s", str(scripts[-1]),
                        "-f", str(ROOT / "tools/openocd.cfg"),
                        "-c", f"adapter serial {serial}", "-c", "init; reset halt"],
                       cwd=ROOT, check=True)
        return 0
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"board: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
