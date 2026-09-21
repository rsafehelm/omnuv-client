#!/usr/bin/env bash
# Builds Omnuv Connect for Ubuntu, Windows and macOS.
#
#     ./packaging/connect/build.sh [version]
#
# Everything is built in containers, so this needs Docker and nothing else on
# the host: no toolchain to install, and no root. Artifacts land in
# dist/connect/.
#
# Unsigned, deliberately, for now. Each platform warns about that in its own
# way and the README says what a person will see rather than pretending they
# will see nothing.
set -euo pipefail

VERSION="${1:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/packaging/connect"
# Beside the thing it builds, like tunnel/dist, and ignored by
# packaging/.gitignore. It was $ROOT/dist/connect while this lived in the
# private repository; the play that ships it reads onv_connect_dist now, so
# the path is declared in one place rather than assumed in two.
OUT="$HERE/dist"
# The deployment this package points at. Overridable, because the address is
# per-deployment and the package is built per-deployment.
MANAGEMENT_URL="${OMNUV_MANAGEMENT_URL:?set OMNUV_MANAGEMENT_URL: the overlay address is per-deployment, and a default bakes one lab into every installer}"
# Where the application signs in. Per-deployment, like the overlay address.
CORE_URL="${OMNUV_CORE_URL:?set OMNUV_CORE_URL: where this deployment Core is}"

rm -rf "$OUT" && mkdir -p "$OUT"
echo "omnuv-connect $VERSION -> overlay $MANAGEMENT_URL, core $CORE_URL"

fill() {
    sed -e "s|@MANAGEMENT_URL@|$MANAGEMENT_URL|g" -e "s|@CORE_URL@|$CORE_URL|g" \
        -e "s|@VERSION@|$VERSION|g" "$1"
}

# ---------- Ubuntu / Debian ----------
build_deb() {
    local stage="$OUT/.deb"
    rm -rf "$stage"
    mkdir -p "$stage/DEBIAN" "$stage/usr/bin" "$stage/usr/share/doc/omnuv-connect"
    fill "$HERE/omnuv-connect" > "$stage/usr/bin/omnuv-connect"
    chmod 0755 "$stage/usr/bin/omnuv-connect"
    # The desktop entry is what makes omnuv:// links in the console clickable.
    mkdir -p "$stage/usr/share/applications"
    cp "$HERE/linux/omnuv-connect.desktop" "$stage/usr/share/applications/"
    cat > "$stage/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
# Tell the desktop a new scheme handler exists. Absent on a server, which is
# fine: there is nothing there to click a link.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
POST
    chmod 0755 "$stage/DEBIAN/postinst"
    cat > "$stage/DEBIAN/control" <<CTL
Package: omnuv-connect
Version: $VERSION
Section: net
Priority: optional
Architecture: all
Depends: curl
Recommends: flatpak
Maintainer: Omnuv <ops@omnuv.com>
Description: Join a device to your Omnuv private network
 One command that installs the tunnel client if it is missing, joins your
 private network with the key from your Omnuv console, and tells you whether
 it worked. Machines are then reachable by name, for example gpu-1.internal.
 .
 The tunnel and game streaming clients are installed from their own
 publishers rather than copied into this package.
CTL
    cp "$HERE/README.md" "$stage/usr/share/doc/omnuv-connect/README" 2>/dev/null || true
    # fakeroot so files are owned by root inside the package without needing it here.
    docker run --rm -v "$OUT:/out" -w /out debian:trixie-slim \
        sh -c "apt-get -qq update >/dev/null && apt-get -qq install -y fakeroot >/dev/null \
               && fakeroot dpkg-deb --build .deb omnuv-connect_${VERSION}_all.deb" >/dev/null
    rm -rf "$stage"
    echo "  deb    $(basename "$OUT"/omnuv-connect_*.deb)"
}

# ---------- Windows ----------
build_exe() {
    local stage="$OUT/.nsis"
    rm -rf "$stage" && mkdir -p "$stage"
    fill "$HERE/omnuv-connect.ps1" > "$stage/omnuv-connect.ps1"
    cp "$HERE/nsis/omnuv-connect.nsi" "$stage/"

    # **The client ships inside the installer, or the installer says it did
    # not.** Until 15 September the package installed *upstream's* Moonlight
    # with `winget`, so a buyer following the product's own path ran a client
    # with none of this repository's work in it. Our build is produced by MSVC
    # on the Windows rig and cannot be made in this Linux container, so it is
    # handed in: `OMNUV_CLIENT_DIR=<windeployqt output>`, which is exactly
    # `build/deploy-x64-release` on the rig and the same tree
    # `lab-windows-install.yml` takes as its artifact.
    #
    # Absent, the installer is wrapper-only and **says so in its name**, so a
    # wrapper-only build can never be mistaken for the shippable one on a
    # directory listing — the failure this is here to prevent is silent.
    local client_files=""
    local suffix=""
    if [ -n "${OMNUV_CLIENT_DIR:-}" ]; then
        [ -x "$OMNUV_CLIENT_DIR/OmnuvClient.exe" ] || {
            echo "  exe    FAILED: no OmnuvClient.exe in OMNUV_CLIENT_DIR=$OMNUV_CLIENT_DIR" >&2
            return 1
        }
        mkdir -p "$stage/client"
        cp -r "$OMNUV_CLIENT_DIR/." "$stage/client/"

        # **The private network, checked rather than assumed.** Neither file is
        # linked — `onvtunneld.exe` is a service the client talks to over a
        # named pipe, and it lazy-loads `wintun.dll` when it creates the
        # adapter — so `windeployqt` copies neither, nothing fails to build
        # without them, and a buyer discovers it at the first join. They are
        # put beside the binary on the rig (`omnuv-build.ps1`); this is the
        # claim that they survived the trip.
        for lib in onvtunneld.exe wintun.dll; do
            [ -f "$stage/client/$lib" ] || {
                echo "  exe    FAILED: $lib is missing from OMNUV_CLIENT_DIR=$OMNUV_CLIENT_DIR" >&2
                echo "         The client would install and report itself unable to reach the network." >&2
                return 1
            }
        done
        # Wintun's licence travels with Wintun: redistribution is permitted
        # only alongside software that uses its published API, and only with
        # its notices intact (clauses 3(c) and 3(d)).
        if [ -f "$ROOT/dist/vendor/wintun/WINTUN-LICENSE.txt" ]; then
            cp "$ROOT/dist/vendor/wintun/WINTUN-LICENSE.txt" "$stage/client/"
        else
            echo "  exe    FAILED: dist/vendor/wintun/WINTUN-LICENSE.txt is missing — run scripts/fetch-wintun" >&2
            return 1
        fi

        client_files="-DCLIENT_DIR=client"
        echo "  exe    including the client from $OMNUV_CLIENT_DIR ($(du -sh "$stage/client" | cut -f1))"
        echo "  exe    with onvtunneld.exe $(stat -c%s "$stage/client/onvtunneld.exe") and wintun.dll $(stat -c%s "$stage/client/wintun.dll")"
    else
        suffix="-noclient"
        echo "  exe    WARNING: no OMNUV_CLIENT_DIR; building a wrapper-only installer" >&2
    fi
    # EnVar puts the install directory on PATH for every user; it is the one
    # plugin this installer needs and it ships with the Debian nsis package.
    docker run --rm -v "$OUT:/out" -w /out/.nsis debian:trixie-slim sh -c "
        set -e
        apt-get -qq update >/dev/null 2>&1
        DEBIAN_FRONTEND=noninteractive apt-get -qq install -y nsis >/dev/null 2>&1
        makensis -DVERSION=$VERSION $client_files -DOUTFILE=/out/OmnuvConnect-${VERSION}${suffix}-setup.exe omnuv-connect.nsi
    " >/dev/null 2>&1 || { echo "  exe    SKIPPED (inputs left in $OUT/.nsis)"; return 0; }
    rm -rf "$stage"
    echo "  exe    OmnuvConnect-${VERSION}${suffix}-setup.exe"
}

# ---------- macOS ----------
#
# A .pkg built without a Mac, using the same tools Apple's own format is made
# of. Unsigned and unnotarised, so Gatekeeper refuses it on a double click and
# the buyer has to open it from the right-click menu once. The README says so.
build_pkg() {
    local stage="$OUT/.pkg"
    local app="$stage/root/Applications/Omnuv Connect.app"
    rm -rf "$stage"
    mkdir -p "$stage/root/usr/local/bin" "$stage/scripts" "$stage/flat/Resources" \
             "$app/Contents/MacOS"
    fill "$HERE/omnuv-connect" > "$stage/root/usr/local/bin/omnuv-connect"
    chmod 0755 "$stage/root/usr/local/bin/omnuv-connect"
    # macOS registers a URL scheme from an application bundle's Info.plist, so
    # a command in /usr/local/bin cannot own one on its own. This bundle exists
    # only to receive the link and hand it over.
    fill "$HERE/macos/Info.plist" > "$app/Contents/Info.plist"
    cp "$HERE/macos/OmnuvConnect" "$app/Contents/MacOS/OmnuvConnect"
    chmod 0755 "$app/Contents/MacOS/OmnuvConnect"
    docker run --rm -v "$OUT:/out" -w /out/.pkg debian:trixie-slim sh -c '
        set -e
        apt-get -qq update >/dev/null 2>&1
        DEBIAN_FRONTEND=noninteractive apt-get -qq install -y xar cpio git g++ make libssl-dev >/dev/null 2>&1
        # Debian has xar but not bomutils, and a .pkg without a Bom is one the
        # macOS installer refuses. It is a small C++ program; building it here
        # is cheaper than requiring a Mac to make a Mac package.
        git clone -q --depth 1 https://github.com/hogliux/bomutils /tmp/bomutils
        make -C /tmp/bomutils -s >/dev/null 2>&1
        /tmp/bomutils/build/bin/mkbom -u 0 -g 80 root Bom
        ( cd root && find . | cpio -o --format odc --quiet ) | gzip -9 > Payload
        files=$( find root -type f | wc -l )
        kb=$( du -sk root | cut -f1 )
        cat > PackageInfo <<INFO
<pkg-info format-version="2" identifier="dev.omnuv.connect" version="'"$VERSION"'" install-location="/" auth="root">
  <payload installKBytes="$kb" numberOfFiles="$files"/>
</pkg-info>
INFO
        cat > Distribution <<DIST
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
  <title>Omnuv Connect</title>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <choices-outline><line choice="default"/></choices-outline>
  <choice id="default"><pkg-ref id="dev.omnuv.connect"/></choice>
  <pkg-ref id="dev.omnuv.connect" version="'"$VERSION"'" onConclusion="none">connect.pkg</pkg-ref>
</installer-gui-script>
DIST
        mkdir -p flat/connect.pkg
        mv Bom PackageInfo Payload flat/connect.pkg/
        mv Distribution flat/
        ( cd flat && xar --compression none -cf /out/OmnuvConnect-'"$VERSION"'.pkg . )
        # Written as root inside the container; clean up here so the host does
        # not need privileges to remove them.
        rm -rf /out/.pkg
        chmod 0644 /out/OmnuvConnect-'"$VERSION"'.pkg
    ' >/dev/null 2>&1 || { echo "  pkg    SKIPPED (inputs left in $OUT/.pkg)"; return 0; }
    rm -rf "$stage" 2>/dev/null || true
    echo "  pkg    OmnuvConnect-${VERSION}.pkg"
}

build_deb
build_exe
build_pkg
echo "artifacts in ${OUT#"$ROOT"/}"
ls -1 "$OUT" 2>/dev/null | sed 's/^/  /'
