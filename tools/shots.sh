#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The screenshots in docs/screens/: the module's own drawing, run on this computer (tools/shots.c). Needs Pillow.
set -e
cd "$(dirname "$0")/.."
git submodule update --init fm1-dock
(cd fm1-dock && { git apply --reverse --check ../patches/fm1-dock-host.patch 2>/dev/null || git apply ../patches/fm1-dock-host.patch; })
mkdir -p build/shots docs/screens
D=fm1-dock
cc -O2 -w -fno-builtin -ffp-contract=off -I$D/firmware/dock -I$D/firmware/src -I$D/firmware/gen -Irhytmsec1 -Irhytmsec1/perf \
    tools/shots.c -o build/shots/shots
build/shots/shots build/shots
python3 - <<'PY'
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/shots").glob("*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(Path("docs/screens") / (p.stem + ".png"))
PY
