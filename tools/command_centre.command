#!/bin/sh
# Local Mac server only. It does not open UART, change firmware or use cloud hosting.
set -eu
command_tools_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$command_tools_dir/.."
command_python=${GARDEN_GUARD_PYTHON:-python3}
exec "$command_python" tools/command_centre.py "$@"
