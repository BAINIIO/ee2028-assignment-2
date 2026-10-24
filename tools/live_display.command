#!/bin/sh
# Double-click on macOS, or run from a terminal. The board runs the game.
set -eu
display_tools_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$display_tools_dir/.."

for display_python in "${GARDEN_GUARD_PYTHON:-}" /opt/anaconda3/bin/python python3; do
    [ -n "$display_python" ] || continue
    if "$display_python" -c 'import tkinter; assert tkinter.TkVersion >= 8.6' >/dev/null 2>&1; then
        exec "$display_python" tools/live_display.py "$@"
    fi
done

printf '%s\n' 'Garden Guard needs Python with Tk 8.6 or later.' >&2
printf '%s\n' 'Set GARDEN_GUARD_PYTHON to that Python executable, then run again.' >&2
exit 1
