#!/usr/bin/env bash
# Builds `onvtunneld`, the privileged half of the private network.
#
#     ./tunnel/build.sh            windows + linux + macOS, into tunnel/dist/
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
case "$targets" in all|windows|linux|macos) ;; *) echo "Unknown tunnel target: $targets" >&2; exit 2 ;; esac

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

# **What these daemons are built from, recorded beside each** (4 October
# 2026). A play shipping dist/ runs source-digest.sh on the tree it ships and
# refuses a daemon whose `<daemon>.BUILT_FROM` names other source:
# lab-windows-build.yml sent the 3 October daemons, without `peers-v1`, beside
# client 7f0f3f5e, and nothing but the files' dates said so. The record also
# carries the daemon's own sha256, so bytes replaced without it are refused
# too. Written inside the container, because dist/ is root's.
digest="$("$here/source-digest.sh")"
repo="$(cd "$here/.." && pwd)"
commit="$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)"
if [ -n "$(git -C "$repo" status --porcelain -- tunnel 2>/dev/null | grep -v ' tunnel/dist/' || true)" ]; then
    dirty=true; tree=none
else
    dirty=false; tree="$(git -C "$repo" rev-parse HEAD:tunnel 2>/dev/null || echo unknown)"
fi
recorded=()
# record <path under tunnel/>: the shell, run in the container after the
# build, that writes that daemon's BUILT_FROM.
record() {
    echo "printf 'source=%s\\ncommit=%s\\ntree=%s\\ndirty=%s\\nsha256=%s\\nbuilt=%s\\n' $digest $commit $tree $dirty \"\$(sha256sum $1 | cut -d' ' -f1)\" \"\$(date -u +%Y-%m-%dT%H:%M:%SZ)\" > $1.BUILT_FROM"
}

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
        run "CGO_ENABLED=0 GOOS=windows GOARCH=$goarch go build -ldflags=\"-s -w\" -o dist/$win/onvtunneld.exe . && $(record "dist/$win/onvtunneld.exe")"
        recorded+=("dist/$win/onvtunneld.exe")
        echo "  windows  dist/$win/onvtunneld.exe  $(du -h "$out/$win/onvtunneld.exe" | cut -f1)"
    done
fi

if [ "$targets" = all ] || [ "$targets" = linux ]; then
    run "CGO_ENABLED=0 go build -ldflags=\"-s -w\" -o dist/onvtunneld . && $(record dist/onvtunneld)"
    recorded+=(dist/onvtunneld)
    echo "  linux    dist/onvtunneld      $(du -h "$out/onvtunneld" | cut -f1)"
fi

# macOS lab artifacts use the existing Unix IPC implementation and an explicit
# private ONV_TUNNEL_DIR. Native service provisioning is owned by Ansible.
if [ "$targets" = all ] || [ "$targets" = macos ]; then
    for pair in x64:amd64 arm64:arm64; do
        mac="${pair%%:*}"; goarch="${pair##*:}"
        run "CGO_ENABLED=0 GOOS=darwin GOARCH=$goarch go build -ldflags=\"-s -w\" -o dist/macos/$mac/onvtunneld . && $(record "dist/macos/$mac/onvtunneld")"
        recorded+=("dist/macos/$mac/onvtunneld")
        echo "  macos    dist/macos/$mac/onvtunneld  $(du -h "$out/macos/$mac/onvtunneld" | cut -f1)"
    done
fi

# **The source did not move while it was built.** An edit during the minutes
# above would leave records naming source the daemons were not built from, so
# they are taken back and the build is refused.
if [ "$("$here/source-digest.sh")" != "$digest" ]; then
    run "rm -f $(printf '%s.BUILT_FROM ' "${recorded[@]}")"
    echo "tunnel/build.sh: tunnel/ changed during the build; its records are removed. Build again." >&2
    exit 3
fi
echo "  source   $digest (commit $commit, dirty=$dirty)"
ls "$out" | sed 's/^/  /'
