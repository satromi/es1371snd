#!/bin/bash
# Build the ES1371 sound driver in the brightv driver (kerext) toolchain.
set -e
SRC=/mnt/c/home/es1371snd
DST=/usr/local/brightv/driver/es1371snd
echo "== copy driver -> $DST =="
rm -rf "$DST"
mkdir -p "$DST/src/device" "$DST/pcat"
cp "$SRC/src/main.c" "$DST/src/"
cp "$SRC/src/device/sound.h" "$DST/src/device/"
cp "$SRC/pcat/Makefile" "$DST/pcat/"
echo "== make (in pcat) =="
cd "$DST/pcat"
make 2>&1 | tail -50
echo "== result =="
ls -la "$DST/pcat/es1371snd" 2>/dev/null && echo "BUILD OK" || echo "no target produced"

echo "== deploy built kerext =="
cp "$DST/pcat/es1371snd" /mnt/c/home/es1371snd/es1371snd 2>/dev/null
mkdir -p /mnt/c/btron-desktop/upload
cp "$DST/pcat/es1371snd" /mnt/c/btron-desktop/upload/es1371snd 2>/dev/null
echo "   deployed: /mnt/c/home/es1371snd/es1371snd"
echo "   deployed: /mnt/c/btron-desktop/upload/es1371snd"
