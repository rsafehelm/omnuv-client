#!/usr/bin/env bash
# Builds `onvtunneld`, the privileged half of the private network.
#
#     ./tunnel/build.sh            windows + linux, into tunnel/dist/
#     ./tunnel/build.sh windows    one of them
#
# **Everything happens in a container on whatever machine you are sitting at.**
# The Windows binary is cross-compiled, so the Windows build rig never needs a
# Go toolchain — it goes on building OmnuvClient.exe with MSVC exactly as it
# does now, and this artefact arrives beside it.
#
# **No cgo, and that is the point of the shape.** This was a `-buildmode=
# c-shared` DLL loaded into the client with QLibrary until 16 September, which
# meant mingw, a C ABI, and a standing rule against allocating on one side and
# freeing on the other. Then the measurement came in: a WireGuard adapter needs
# administrator rights, the client does not have them at login, and the tunnel
# had to move into a service anyway. A service talks over a socket, so the C
# surface had nothing left to do — and a pure-Go static binary is the simplest
# thing that can possibly work.
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

# The one check worth its second: the protocol answers what it says it answers,
# including the two refusals that are questions rather than retries.
run 'go vet . && go test .'

# **One directory per architecture, named the way the installer names them.**
# `x64` and `arm64` are MSBuild's `$(Platform)` and `build-arch.bat`'s `%ARCH%`
# verbatim, so `TunnelDir` is one path with the architecture substituted and
# nothing has to translate between two vocabularies.
#
# Until 17 September this produced a single amd64 `dist/onvtunneld.exe` and the
# installer had no architecture in its path at all, so an arm64 MSI would have
# carried an x64 daemon that cannot run. It failed no build and no x64 install,
# which is why it survived: the only machine that would have noticed is one
# nobody had built for yet.
if [ "$targets" = all ] || [ "$targets" = windows ]; then
    for pair in x64:amd64 arm64:arm64; do
        win="${pair%%:*}"; goarch="${pair##*:}"
        run "CGO_ENABLED=0 GOOS=windows GOARCH=$goarch go build -ldflags=\"-s -w\" -o dist/$win/onvtunneld.exe ."
        echo "  windows  dist/$win/onvtunneld.exe  $(du -h "$out/$win/onvtunneld.exe" | cut -f1)"
    done
fi

if [ "$targets" = all ] || [ "$targets" = linux ]; then
    run 'CGO_ENABLED=0 go build -ldflags="-s -w" -o dist/onvtunneld .'
    echo "  linux    dist/onvtunneld      $(du -h "$out/onvtunneld" | cut -f1)"
fi

ls "$out" | sed 's/^/  /'
