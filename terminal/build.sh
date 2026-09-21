#!/usr/bin/env bash
# Builds `omnuv`, the marketplace from a terminal.
#
#     ./terminal/build.sh            linux + windows, into terminal/dist/
#     ./terminal/build.sh linux      one of them
#
# **Everything happens in a container**, like `tunnel/build.sh` beside it, so a
# release does not depend on what is installed on the machine you are sitting
# at, and the Windows build rig never needs a Rust toolchain.
#
# **macOS is not built anywhere yet, and this script does not pretend
# otherwise.** `tunnel/` reaches all three targets because Go cross-compiles
# with two environment variables. This crate pulls `ring` through rustls, which
# wants a C toolchain per target: mingw covers Windows from Linux, and Apple's
# does not leave the platform.
#
# The obvious answer -- build it on `onv-dev-mac` -- is wrong twice, and this
# comment said it for an afternoon before anybody checked. That rig's toolchain
# is clang, the macOS SDK, node, create-dmg and Qt; there is no Rust on it. And
# `docs/lab-macos.md` in the private repository is explicit that the rig is for
# running what it builds, never for building a release: no paid Apple Developer
# Program means no notarised artefact, and an artefact built there would carry
# the host that produced it.
#
# A `macos-*` GitHub runner builds this natively and free, which is what a CLI
# wants -- it is one binary, not a `.dmg` that Gatekeeper quarantines. Queued
# in the private repository rather than guessed at here.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/dist"; mkdir -p "$out"
targets="${1:-all}"
case "$targets" in all|linux|windows) ;; *) echo "Unknown terminal target: $targets" >&2; exit 2 ;; esac

# The registry and the target directory are volumes, so a second run is fast
# and nothing is written into the checkout as root.
run() {
    docker run --rm \
        -v "$here:/w" \
        -v omnuv_cargo_registry:/usr/local/cargo/registry \
        -v omnuv_terminal_target:/w/target \
        -w /w rust:1 bash -c "$1"
}

# The one check worth its second: it compiles and the tests such as they are
# pass. A build script that has never run the thing it ships is a wish.
run 'cargo test --offline 2>/dev/null || cargo test'

if [ "$targets" = all ] || [ "$targets" = linux ]; then
    run 'cargo build --release && install -m755 target/release/omnuv dist/omnuv'
    echo "  linux    dist/omnuv        $(du -h "$out/omnuv" | cut -f1)"
fi

# **One directory per architecture, named the way the installer names them** —
# `x64` is MSBuild's `$(Platform)` and `build-arch.bat`'s `%ARCH%` verbatim, so
# the installer needs no translation. arm64-pc-windows-gnu has no stable Rust
# std, so x64 is what there is; an arm64 Windows machine runs it emulated.
if [ "$targets" = all ] || [ "$targets" = windows ]; then
    run 'apt-get -qq update && apt-get -qq install -y mingw-w64 >/dev/null &&
         rustup target add x86_64-pc-windows-gnu &&
         cargo build --release --target x86_64-pc-windows-gnu &&
         mkdir -p dist/x64 &&
         install -m755 target/x86_64-pc-windows-gnu/release/omnuv.exe dist/x64/omnuv.exe'
    echo "  windows  dist/x64/omnuv.exe  $(du -h "$out/x64/omnuv.exe" | cut -f1)"
fi
