#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build RHYTM-SEC-1's firmware: fm1-dock (the submodule, at the commit this repo pins) with this repo's patch, as a
# solo app with the module built in. Needs what fm1-dock's build needs: Docker on macOS, the JieLi toolchain
# (fm1-dock/tools/get_toolchain.sh) and the AC79 SDK (see fm1-dock's README).
# Out: build/rhytm-sec-1-VERSION.fwsc (the standalone firmware) and build/rhytmsec1.dock (the module for fm1-dock)
set -e
cd "$(dirname "$0")"
VERSION="${VERSION:-1.1}"
git submodule update --init fm1-dock
cd fm1-dock
git apply --reverse --check ../patches/fm1-dock-host.patch 2>/dev/null || git apply ../patches/fm1-dock-host.patch
rm -rf modules/rhytmsec1 && cp -R ../rhytmsec1 modules/rhytmsec1
./build.sh --release "$VERSION" --modules rhytmsec1 --name rhytm-sec-1 --app-name RHYTM-SEC-1 --identity FM-1_947 \
    --uac-stems 4 --uac-full
mkdir -p ../build
cp "build/rhytm-sec-1-$VERSION.fwsc" build/modules/rhytmsec1.dock ../build/
echo "firmware -> build/rhytm-sec-1-$VERSION.fwsc"
echo "module   -> build/rhytmsec1.dock"
