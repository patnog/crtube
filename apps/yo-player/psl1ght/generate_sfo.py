#!/usr/bin/env python3
"""Generate the PSL1GHT PARAM.SFO XML from the app's semantic version."""
import re
import sys
from pathlib import Path

if len(sys.argv) != 4:
    raise SystemExit("usage: generate_sfo.py TEMPLATE VERSION_FILE OUTPUT")

template, version_file, output = map(Path, sys.argv[1:])
version = version_file.read_text(encoding="ascii").strip()
match = re.fullmatch(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version)
if not match:
    raise SystemExit(f"invalid semantic version in {version_file}: {version!r}")
major, minor, patch = map(int, match.groups())
# APP_VER has two digits on either side of the dot. Encode MAJOR as the
# first pair and the clamped MINOR/PATCH digits as the second pair:
# 1.0.10 -> 01.09, 1.12.15 -> 01.99. Clamp each XMB component independently.
major = min(major, 99)
minor = min(minor, 9)
patch = min(patch, 9)
ps3_version = f"{major:02d}.{minor}{patch}"
text = template.read_text(encoding="utf-8")
text = re.sub(r'(<value name="APP_VER" type="string">)[^<]*(</value>)',
              rf'\g<1>{ps3_version}\g<2>', text)
text = re.sub(r'(<value name="VERSION" type="string">)[^<]*(</value>)',
              rf'\g<1>{ps3_version}\g<2>', text)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(text, encoding="utf-8")
print(f"PARAM.SFO version: {ps3_version} (CRTube {version})")
