#!/usr/bin/env bash
# Builds the embedded tunnel as a shared library the client loads at run time.
#
#     ./tunnel/build.sh            windows + linux, into tunnel/dist/
#     ./tunnel/build.sh windows    one of them
#
# **Everything happens in a container on whatever machine you are sitting at.**
# The Windows DLL is cross-compiled with mingw, so the Windows build rig never
# needs a Go toolchain — it goes on building OmnuvClient.exe with MSVC exactly
# as it does now, and this artefact arrives beside it.
#
# The two halves meet at run time rather than at link time: this emits a .dll
# and a .h and no import library, so MSVC has nothing to link against and needs
# nothing — `QLibrary::resolve()` loads the plain C exports by name. That also
# sidesteps the MSVC-against-mingw ABI question entirely.
#
# `-s -w` because the symbol tables are half the file: 62.9 MB becomes 32.2 MB,
# measured, against a client that already ships 126 MB.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/dist"; mkdir -p "$out"
targets="${1:-all}"

run() { docker run --rm -v "$here:/w" -v omnuv_go_mod:/go/pkg/mod -w /w golang:1.26 bash -c "$1"; }

# **go.sum is generated once and committed**, so a build is reproducible and
# does not reach for whatever the proxy serves today. `go mod tidy` is what
# writes it, and it only works because go.mod repeats NetBird's nine `replace`
# directives verbatim: a dependency's replaces are ignored when it is consumed
# as a library, so without them pion/ice resolves to a placeholder version and
# tidy dies resolving NetBird's Dex and Rosenpass packages, which have nothing
# to do with the tunnel.
if [ ! -f "$here/go.sum" ]; then
    echo "  go.sum missing — generating it once"
    run 'go mod tidy'
fi

if [ "$targets" = all ] || [ "$targets" = windows ]; then
    run '
      apt-get -qq update >/dev/null 2>&1
      DEBIAN_FRONTEND=noninteractive apt-get -qq install -y gcc-mingw-w64-x86-64 >/dev/null 2>&1
      CGO_ENABLED=1 GOOS=windows GOARCH=amd64 CC=x86_64-w64-mingw32-gcc \
        go build -buildmode=c-shared -ldflags="-s -w" -o dist/onvtunnel.dll .
    '
    echo "  windows  dist/onvtunnel.dll  $(du -h "$out/onvtunnel.dll" | cut -f1)"
fi

if [ "$targets" = all ] || [ "$targets" = linux ]; then
    run 'CGO_ENABLED=1 go build -buildmode=c-shared -ldflags="-s -w" -o dist/onvtunnel.so .'
    echo "  linux    dist/onvtunnel.so   $(du -h "$out/onvtunnel.so" | cut -f1)"
fi

# The header is the contract the C++ side resolves against; it is generated
# beside whichever library was built last and is identical for both.
ls "$out" | sed 's/^/  /'
