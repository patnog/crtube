#!/usr/bin/env python3
"""Generate the C version header from the repository's semantic VERSION file."""
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: generate_version_header.py VERSION_FILE OUTPUT_HEADER")

version_file, output = map(Path, sys.argv[1:])
version = version_file.read_text(encoding="ascii").strip()
match = re.fullmatch(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version)
if not match:
    raise SystemExit(f"invalid semantic version in {version_file}: {version!r}")
# Keep the full semantic version for the UI and GitHub comparisons. The
# APP_VER limits are applied only by generate_sfo.py.
if version.startswith("v"):
    version = version[1:]
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(f'#pragma once\n#define CRTUBE_VERSION "{version}"\n', encoding="ascii")
print(f"CRTube version header: {version}")
