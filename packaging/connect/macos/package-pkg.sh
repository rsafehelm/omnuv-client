#!/bin/bash
# One .pkg for macOS: the client with its Qt, the tunnel daemon under launchd,
# and omnuv-connect. The counterpart of linux/package-deb.sh, and the
# operator's decision of 24 September: one package, as on Linux and Windows.
#
#     package-pkg.sh --app OmnuvClient.app --daemon onvtunneld \
#                    --connect omnuv-connect --version V --qt-bin DIR \
#                    --qmldir DIR --out DIR
#
# **Runs on a Mac**, because macdeployqt and pkgbuild are Apple-only tools:
# on CI's hosted macOS runners for anything that ships, or on the lab rig to
# test. For the closed test the package people download is the lab rig's, by
# the operator's decision of 24 September 2026 (lab-macos.yml's header, the
# licence).
#
# Layout it installs:
#
#     /Applications/Omnuv.app                              the client, Qt inside
#     /Library/Application Support/Omnuv/onvtunneld        the daemon
#     /Library/LaunchDaemons/dev.omnuv.tunnel.plist        started at install, and at boot
#     /usr/local/bin/omnuv-connect
#
# **Proof, not hope: the bundle stands alone.** After macdeployqt, every
# Mach-O in the bundle is read with otool, and one that still names the Qt it
# was built against, outside the bundle, fails the package. That is the check
# stage-client.sh makes with ldd on Linux.
set -euo pipefail
app='' daemon='' connect='' version='' qt_bin='' qmldir='' out=''
while [ $# -gt 0 ]; do
    case "$1" in
        --app) app=$2 ;; --daemon) daemon=$2 ;; --connect) connect=$2 ;;
        --version) version=$2 ;; --qt-bin) qt_bin=$2 ;; --qmldir) qmldir=$2 ;; --out) out=$2 ;;
        *) echo "package-pkg: unknown argument $1" >&2; exit 64 ;;
    esac
    shift 2
done
for v in app daemon connect version qt_bin qmldir out; do
    [ -n "${!v}" ] || { echo "package-pkg: --${v//_/-} is required" >&2; exit 64; }
done
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
root="$work/root"
mkdir -p "$root/Applications" "$root/Library/Application Support/Omnuv" \
    "$root/Library/LaunchDaemons" "$root/usr/local/bin" "$work/scripts"

cp -R "$app" "$root/Applications/Omnuv.app"
"$qt_bin/macdeployqt" "$root/Applications/Omnuv.app" -qmldir="$qmldir" -always-overwrite >"$work/macdeployqt.log" 2>&1 \
    || { cat "$work/macdeployqt.log" >&2; exit 1; }

qt_prefix="$(cd "$qt_bin/.." && pwd)"
leaks=$(find "$root/Applications/Omnuv.app" -type f -perm -u+x -print0 \
    | xargs -0 -I{} sh -c 'otool -L "{}" 2>/dev/null | tail -n +2' \
    | grep -F "$qt_prefix" | sort -u || true)
[ -z "$leaks" ] || { echo "the bundle still reaches outside itself for Qt:" >&2; echo "$leaks" >&2; exit 1; }
core=$(otool -L "$root/Applications/Omnuv.app/Contents/MacOS/OmnuvClient" | grep -c 'QtCore' || true)
[ "$core" -ge 1 ] || { echo "OmnuvClient links no QtCore at all, which cannot be right" >&2; exit 1; }
# macdeployqt rewrote the load commands, which breaks any signature; an
# ad-hoc one is the least the loader accepts on Apple silicon.
codesign --force --deep --sign - "$root/Applications/Omnuv.app" >/dev/null 2>&1

install -m 0755 "$daemon" "$root/Library/Application Support/Omnuv/onvtunneld"
install -m 0644 "$here/dev.omnuv.tunnel.plist" "$root/Library/LaunchDaemons/dev.omnuv.tunnel.plist"
install -m 0755 "$connect" "$root/usr/local/bin/omnuv-connect"

cat > "$work/scripts/preinstall" <<'PRE'
#!/bin/sh
# An earlier version's daemon stops before its files are replaced.
launchctl bootout system/dev.omnuv.tunnel 2>/dev/null || true
# The earlier package's link-only bundle claimed omnuv:// too, and two
# claimants leave which one opens a link to LaunchServices. The client in
# Omnuv.app registers the scheme now.
rm -rf "/Applications/Omnuv Connect.app"
exit 0
PRE
# The postinstall is a file beside this script, so its test can run it: it
# records the installing person as the tunnel's owner, and starts the daemon.
cp "$(dirname "$0")/postinstall" "$work/scripts/postinstall"
chmod 0755 "$work/scripts/preinstall" "$work/scripts/postinstall"

mkdir -p "$out"
pkg="$out/OmnuvConnect-$version.pkg"
# **Not relocatable** (25 September 2026). By default Installer "upgrades" a
# bundle it finds registered anywhere with the same identifier, so on the lab
# Mac, where the build tree's copy had been opened once, the install went into
# ~/client/build/app and /Applications/Omnuv.app never existed. A person with
# an older copy somewhere would have it replaced in place, wherever it is.
pkgbuild --analyze --root "$root" "$work/components.plist" >/dev/null
count=$(/usr/libexec/PlistBuddy -c 'Print' "$work/components.plist" | grep -c 'BundleIsRelocatable')
[ "$count" -ge 1 ] || { echo "no bundle in the package to pin" >&2; exit 1; }
i=0
while [ "$i" -lt "$count" ]; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$work/components.plist"
    i=$((i + 1))
done
pkgbuild --root "$root" --component-plist "$work/components.plist" --scripts "$work/scripts" \
    --identifier dev.omnuv.connect --version "$version" --install-location / --ownership recommended "$pkg" >/dev/null
echo "PKG=$pkg"
echo "SIZE=$(du -h "$pkg" | cut -f1)"
