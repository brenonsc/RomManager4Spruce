#!/bin/sh
# Builds RomManager's programs for spruce's 64-bit handhelds into RomManager/bin:
# static aarch64, compiled in Docker (nothing else to install).
#   sh src/build.sh            rmui, rmkey, rmaudio and rmls
#   sh src/build.sh preview    rmui for this computer, to draw frames as PNG:
#                              src/rmui-preview --preview 1024 768 out.png frame.txt
cd "$(dirname "$0")" || exit 1
if [ "$1" = preview ]; then
    cc -O2 -o rmui-preview rmui.c -lm -lpthread && echo "built src/rmui-preview"
    exit
fi
docker run --rm -v "$PWD/..:/w" -w /w/src arm64v8/alpine:3.20 sh -c '
    apk add --no-cache gcc musl-dev linux-headers >/dev/null || exit 1
    for t in rmui rmkey rmaudio rmls; do
        gcc -O2 -static -s -Wno-format-truncation "$t.c" -o "../RomManager/bin/$t" -lm -lpthread || exit 1
    done' && echo "built RomManager/bin: rmui rmkey rmaudio rmls"
