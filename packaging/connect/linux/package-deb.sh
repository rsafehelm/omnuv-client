#!/usr/bin/env bash
# One .deb: the client, its Qt, the tunnel service and omnuv-connect.
#
#     package-deb.sh <staged-client> <onvtunneld> <omnuv-connect> <version> <out-dir>
#
# Runs as root inside the client-linux image (Ubuntu 26.04), because
# dpkg-shlibdeps must see the same system libraries the client was built
# against: that is how the package's Depends are worked out rather than
# written by hand. The operator's decision, 24 September 2026: one package.
#
# Layout:
#
#     /opt/omnuv/bin/OmnuvClient       a wrapper: the bundled Qt, first
#     /opt/omnuv/bin/OmnuvClient.bin   the client (stage-client.sh)
#     /opt/omnuv/bin/onvtunneld        the private-network daemon, as root
#     /opt/omnuv/{lib,plugins,qml}     the Qt it needs, and nothing else
#     /usr/bin/OmnuvClient, /usr/bin/omnuv-connect
#     /usr/lib/systemd/system/onv-tunnel.service
#     /usr/share/applications/         the app, and the omnuv:// handler
#
# What a removal keeps: /var/lib/onv/tunnel, the device's identity. Removing
# it would make a reinstall enrol again and leave the old peer in the buyer's
# network; the peer is revoked from the console, as on Windows.
set -euo pipefail
staged="$1"; daemon="$2"; connect="$3"; version="$4"; out="$5"
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
root="$(mktemp -d)"

install -d "$root/opt/omnuv" "$root/usr/bin" "$root/usr/lib/systemd/system" \
    "$root/usr/share/applications" "$root/DEBIAN"
cp -a "$staged/." "$root/opt/omnuv/"
install -m 0755 "$daemon" "$root/opt/omnuv/bin/onvtunneld"
cat > "$root/opt/omnuv/bin/OmnuvClient" <<'WRAP'
#!/bin/sh
# The bundled Qt before any on the system: the client was built against
# 6.11.2, and a distribution's Qt of another minor version is not it.
here=/opt/omnuv
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/bin/OmnuvClient.bin" "$@"
WRAP
chmod 0755 "$root/opt/omnuv/bin/OmnuvClient"
ln -s /opt/omnuv/bin/OmnuvClient "$root/usr/bin/OmnuvClient"
install -m 0755 "$connect" "$root/usr/bin/omnuv-connect"
install -m 0644 "$here/omnuv-connect.desktop" "$root/usr/share/applications/"
install -m 0644 "$here/omnuv-client.desktop" "$root/usr/share/applications/"
install -m 0644 "$here/onv-tunnel.service" "$root/usr/lib/systemd/system/"
install -D -m 0644 "$here/../../../app/omnuv/omnuv.svg" "$root/usr/share/icons/hicolor/scalable/apps/omnuv.svg"

# Depends, from the binaries themselves. The bundled Qt resolves from
# /opt/omnuv/lib; what dpkg-shlibdeps reports is what the system must supply.
mkdir -p "$root/debian"
printf 'Source: omnuv-connect\n\nPackage: omnuv-connect\nArchitecture: amd64\n' > "$root/debian/control"
# Its errors are kept and shown: hidden, a failure here read as an empty
# Depends with no reason (seen 24 September 2026).
depends=$(cd "$root" && find opt/omnuv -type f \( -name '*.so*' -o -name 'OmnuvClient.bin' \) -print0 \
    | xargs -0 dpkg-shlibdeps -O -l"$root/opt/omnuv/lib" --ignore-missing-info 2>"$root/shlibdeps.err" \
    | sed -n 's/^shlibs:Depends=//p') || { cat "$root/shlibdeps.err" >&2; exit 1; }
rm -f "$root/shlibdeps.err"
# **Not the system's Qt.** The QML plugins resolve Qt from /usr/lib in the
# build image (it has Ubuntu's Qt 6.10), so dpkg-shlibdeps names libqt6*
# packages (and qml6-module-*, qt6-*-private-abi); at run time the wrapper puts /opt/omnuv/lib first and the bundled
# 6.11.2 is what loads. Dropped, and proved by installing on an image with no
# Qt at all (24 September 2026).
depends=$(printf '%s' "$depends" | tr ',' '\n' | sed 's/^ *//' | grep -vE '^(libqt6|qml6-module-|qt6-)' | paste -sd, - | sed 's/,/, /g')
rm -rf "$root/debian"
[ -n "$depends" ] || { echo "dpkg-shlibdeps found no dependencies, which cannot be right" >&2; exit 1; }

cat > "$root/DEBIAN/control" <<CTL
Package: omnuv-connect
Version: $version
Section: net
Priority: optional
Architecture: amd64
Depends: $depends
Maintainer: Omnuv <ops@omnuv.com>
Description: Omnuv on this device: the app and its private network
 The Omnuv client, the service that holds this device on its private network,
 and omnuv-connect, the command that joins it with a key from the Omnuv
 console. Machines are then reachable by name, for example gpu-1.internal.
 Built for Ubuntu 26.04.
CTL
# No `Conflicts: netbird`: the preinst beside this script refuses in words
# instead (the operator's decision of 24 September 2026), and a conflict would
# answer first, with a solver dump or by removing a person's own netbird.
install -m 0755 "$here/preinst" "$root/DEBIAN/preinst"
# The postinst is a file beside this script, so its test can run it: it
# records the installing person as the tunnel's owner, and enables the
# service.
install -m 0755 "$here/postinst" "$root/DEBIAN/postinst"
cat > "$root/DEBIAN/prerm" <<'PRERM'
#!/bin/sh
set -e
# Stopped before its files go. The identity under /var/lib/onv stays.
if [ -d /run/systemd/system ] && [ "$1" = remove ]; then
    systemctl disable --now onv-tunnel.service || true
fi
PRERM
chmod 0755 "$root/DEBIAN/prerm"

mkdir -p "$out"
dpkg-deb --root-owner-group --build "$root" "$out/omnuv-connect_${version}_amd64.deb" >/dev/null
rm -rf "$root"
echo "DEB=$out/omnuv-connect_${version}_amd64.deb"
echo "DEPENDS=$depends"
