#!/bin/bash
# Build the ES1371 driver in TIMER-REFILL mode (REFILL_BY_TIMER=1): interrupt-free
# playback -- the DAC2 interrupt is DISABLED and the ring is refilled from a cyclic
# handler, so the VMware INTx-storm freeze cannot occur. The repo source keeps
# REFILL_BY_TIMER 0 (the proven IRQ+watchdog default); this script flips it to 1 in
# the build copy only. Use build-es1371.sh to go back to the safe default.
set -e
SRC=/mnt/c/home/es1371snd
DST=/usr/local/brightv/driver/es1371snd
echo "== copy driver -> $DST (TIMER mode) =="
rm -rf "$DST"
mkdir -p "$DST/src/device" "$DST/pcat"
cp "$SRC/src/main.c" "$DST/src/"
cp "$SRC/src/device/sound.h" "$DST/src/device/"
cp "$SRC/pcat/Makefile" "$DST/pcat/"
sed -i 's/#define REFILL_BY_TIMER 0/#define REFILL_BY_TIMER 1/' "$DST/src/main.c"
grep -n "define REFILL_BY_TIMER" "$DST/src/main.c" | head -1
echo "== make (in pcat) =="
cd "$DST/pcat"
make 2>&1 | tail -50
echo "== result =="
ls -la "$DST/pcat/es1371snd" 2>/dev/null && echo "BUILD OK (TIMER)" || echo "no target produced"
echo "== deploy built kerext =="
cp "$DST/pcat/es1371snd" /mnt/c/home/es1371snd/es1371snd 2>/dev/null
mkdir -p /mnt/c/btron-desktop/upload
cp "$DST/pcat/es1371snd" /mnt/c/btron-desktop/upload/es1371snd 2>/dev/null
echo "   deployed (TIMER): /mnt/c/home/es1371snd/es1371snd"
echo "   deployed (TIMER): /mnt/c/btron-desktop/upload/es1371snd"
