#!/usr/bin/env python3
"""Track compiler/linker switches without rebuilding unchanged configurations."""
from pathlib import Path
import sys

path = Path(sys.argv[1])
content = "\n".join(sys.argv[2:]) + "\n"
path.parent.mkdir(parents=True, exist_ok=True)
if not path.exists() or path.read_text() != content:
    path.write_text(content)
