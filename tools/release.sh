#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# A release: build both, then a GitHub release vVERSION with the standalone firmware and the fm1-dock module.
#   tools/release.sh 1.2 [notes-file]
set -e
cd "$(dirname "$0")/.."
[ -n "$1" ] || { echo "usage: tools/release.sh X.Y [notes-file]"; exit 1; }
VERSION="$1" ./build.sh
FW="build/rhytm-sec-1-$1.fwsc"
[ -f "$FW" ] && [ -f build/rhytmsec1.dock ] || { echo "release: missing $FW or build/rhytmsec1.dock"; exit 1; }
# the browser version (fmhub.json's "play": fm1-emu's ?package=) plays the copy on Pages: releases can't be fetched cross-origin
cp "$FW" docs/rhytm-sec-1.fwsc
git add docs/rhytm-sec-1.fwsc && { git diff --cached --quiet || git commit -q -m "Pages: the browser version is $1"; } && git push -q
case "$1" in *.*.*) TAG="v$1" ;; *) TAG="v$1.0" ;; esac   # (1.2 -> v1.2.0, as the tags so far)
if [ -n "$2" ]; then NOTES="--notes-file=$2"; else NOTES="--generate-notes"; fi
gh release create "$TAG" "$FW" build/rhytmsec1.dock --title "RHYTM-SEC-1 $1" "$NOTES"
