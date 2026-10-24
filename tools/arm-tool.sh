#!/bin/sh
set -eu

usage() {
    printf 'Usage: %s gcc|gdb|size|objdump|objcopy [arguments...]\n' "$0"
    printf '%s\n' \
        'Uses arm-none-eabi tools from PATH or an installed macOS STM32CubeIDE.' \
        'From the Assignment 2 directory, run make to build the firmware.' \
        'See README.md for project setup, build and board instructions.'
}

case "${1:-}" in
    gcc|gdb|size|objdump|objcopy) tool=$1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

if command -v "arm-none-eabi-$tool" >/dev/null 2>&1; then
    exec "arm-none-eabi-$tool" "$@"
fi

# Reuse STM32CubeIDE's bundled toolchain without modifying the user's PATH.
for executable in \
    /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin/arm-none-eabi-"$tool"; do
    if [ -x "$executable" ]; then
        exec "$executable" "$@"
    fi
done

printf 'ARM tool not found: arm-none-eabi-%s\n' "$tool" >&2
printf '%s\n' \
    'Install STM32CubeIDE, or put an Arm GNU Toolchain bin directory on PATH.' \
    'See the Assignment 2 README.md for setup; this script does not install tools.' >&2
exit 1
