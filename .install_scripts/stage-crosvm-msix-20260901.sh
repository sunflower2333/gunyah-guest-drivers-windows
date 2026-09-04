#!/system/bin/sh
set -eu

stage=/data/data/com.termux/files/home/crosvm-context-diagnostics-e7433abd
mkdir -p "$stage"
cp /data/local/tmp/crosvm-e7433abd.new "$stage/crosvm"
cp /data/local/tmp/libvirglrenderer-e7433abd.new "$stage/libvirglrenderer.so"
chmod 755 "$stage/crosvm" "$stage/libvirglrenderer.so"
sha256sum "$stage/crosvm" "$stage/libvirglrenderer.so"
