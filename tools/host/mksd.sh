#!/bin/sh
# Make an SD-card image for the host build of pico-speccy (host/): a FAT32 volume,
# optionally with files copied into its root.
#   tools/host/mksd.sh card.img [size_mb] [file ...]
# Needs dosfstools (mkfs.vfat) and mtools (mcopy). Run the emulator with
#   pico-speccy --sd card.img
set -e
img=${1:?usage: mksd.sh card.img [size_mb] [file ...]}
size=${2:-256}
[ $# -ge 2 ] && shift 2 || shift 1
rm -f "$img"
dd if=/dev/zero of="$img" bs=1M count=0 seek="$size" 2>/dev/null
mkfs.vfat -F 32 -n PICOSPECCY "$img" >/dev/null
for f in "$@"; do mcopy -i "$img" "$f" ::/; done
echo "$img: ${size} MB FAT32"
