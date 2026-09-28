#!/bin/sh
# package.sh: build/superdrastic-<version>-aarch64.tar.gz from build/libsuperdrastic.so (run build.sh first)
set -e
cd "$(dirname "$0")"
V=$(cat VERSION)
D=build/superdrastic-$V-aarch64
[ -f build/libsuperdrastic.so ] || { echo "no build/libsuperdrastic.so: run build.sh first"; exit 1; }
rm -rf "$D"; mkdir -p "$D/shaders"
cp build/libsuperdrastic.so runner/superdrastic-run runner/superdrastic-update runner/superdrastic.conf.example \
   VERSION README.md LICENSE THIRD_PARTY.md "$D/"
cp shaders/*.frag "$D/shaders/"
mkdir -p "$D/docs" && cp docs/INTEGRATION.md "$D/docs/"
chmod +x "$D/superdrastic-run" "$D/superdrastic-update"
tar czf "$D.tar.gz" -C build "superdrastic-$V-aarch64"
ls -l "$D.tar.gz"
