#!/bin/sh

# The ImageMagick conversion tool doesn't seem to always generate
# ICO files with background transparency properly. Please validate
# that the output has a transparent background.

convert -density 256 -background none -define icon:auto-resize ../app/res/moonlight.svg ../app/moonlight.ico
convert -density 256 -background none -size 64x64 ../app/res/moonlight.svg ../app/moonlight_wix.png

echo IMPORTANT: Validate the icon has a transparent background before committing!

# ---- Omnuv ----------------------------------------------------------------
#
# Our own mark, and the application's icon since 15 September. Regenerate it
# the same way it was made, in a container so no toolchain is needed on the
# host and the result does not depend on whose machine ran it:
#
#   docker run --rm -v "$PWD:/src" -w /src debian:trixie-slim bash -c '
#     apt-get -qq update && apt-get -qq install -y librsvg2-bin imagemagick icnsutils
#     for s in 16 20 24 32 40 48 64 128 256 512; do
#         rsvg-convert -w $s -h $s -o /tmp/$s.png app/omnuv/omnuv.svg
#     done
#     magick /tmp/16.png /tmp/20.png /tmp/24.png /tmp/32.png /tmp/40.png \
#            /tmp/48.png /tmp/64.png /tmp/128.png /tmp/256.png app/omnuv.ico
#     png2icns app/omnuv.icns /tmp/16.png /tmp/32.png /tmp/48.png \
#              /tmp/128.png /tmp/256.png /tmp/512.png'
#
# `rsvg-convert` rather than ImageMagick's own SVG path: ImageMagick 7
# delegates SVG to it and fails outright when it is absent, and its internal
# renderer does not round `rx` corners the way the mark is drawn.
