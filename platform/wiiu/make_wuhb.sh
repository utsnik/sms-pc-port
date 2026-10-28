#!/bin/bash
# Package sms.rpx as a .wuhb: ./make_wuhb.sh [rpx] [out.wuhb]
# The game reads its data from the ABSOLUTE SD path /vol/external01/wiiu/apps/sms/
# (see runtime.cpp), so the .wuhb carries only the RPX and art.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RPX="${1:-$HERE/../../build/wiiu/sms.rpx}"
OUT="${2:-${RPX%.rpx}.wuhb}"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
cp "$RPX" "$STAGE/root.rpx"; cp "$HERE"/wuhb/*.png "$STAGE/"
docker run --rm --user "$(id -u):$(id -g)" -v "$STAGE:/w" -w /w soh-wiiu-builder:latest \
  /opt/devkitpro/tools/bin/wuhbtool root.rpx out.wuhb --name="Super Mario Sunshine (port)" \
  --short-name="SMS" --author="sms-pc-port" --icon=icon.png --tv-image=tv.png --drc-image=drc.png
head -c 4 "$STAGE/out.wuhb" | grep -q WUHB || { echo "BAD: no WUHB magic" >&2; exit 1; }
cp "$STAGE/out.wuhb" "$OUT"; echo "built $OUT"
