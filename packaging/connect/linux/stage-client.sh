#!/usr/bin/env bash
# The Linux client and the Qt it needs, as one directory for /opt/omnuv.
#
#     stage-client.sh <out-dir>
#
# Runs inside the client-linux image (deployment/client-linux/Dockerfile in
# the omnuv repository), after the client is built into /build, with the
# pinned Qt mounted at $OMNUV_QT. Writes:
#
#     <out>/bin/OmnuvClient.bin   the client
#     <out>/bin/qt.conf           where its plugins and QML are, relative to it
#     <out>/lib/                  every Qt library it, its plugins and its QML
#                                 modules load, and nothing else of Qt's
#     <out>/plugins/, <out>/qml/  the plugins and the QML modules it imports
#
# **Why not the AppImage.** The operator chose one .deb holding the client,
# its Qt and the tunnel service (24 September 2026). An AppImage cannot carry
# a system service, and the tunnel has to be one: making a WireGuard adapter
# needs root, and the client runs as the person.
#
# **Found, not listed.** Libraries by `ldd`, QML modules by Qt's own
# `qmlimportscanner` over the sources. A hand-written list is the one that is
# wrong the day a page imports something new. System libraries (glibc, X,
# GL, SDL, ffmpeg) are not copied: the package declares them as dependencies,
# worked out by dpkg-shlibdeps from what is left.
set -euo pipefail
out="${1:?usage: stage-client.sh <out-dir>}"
qt="${OMNUV_QT:?OMNUV_QT is not set}"
bin=/build/app/OmnuvClient
[ -x "$bin" ] || { echo "no built client at $bin" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/bin" "$out/lib" "$out/plugins" "$out/qml"
cp "$bin" "$out/bin/OmnuvClient.bin"

# The plugin families a desktop Qt Quick client loads on X11 and Wayland.
for dir in platforms xcbglintegrations platforminputcontexts platformthemes iconengines imageformats tls \
           networkinformation wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration \
           egldeviceintegrations generic; do
    [ -d "$qt/plugins/$dir" ] && cp -r "$qt/plugins/$dir" "$out/plugins/"
done

# Not the GTK3 theme: the client draws its own style, and that plugin would
# make GTK a dependency of the whole package (dpkg-shlibdeps refused it on
# 24 September 2026, the image having no GTK to resolve it against).
rm -f "$out/plugins/platformthemes/libqgtk3.so"

# The QML modules the client's own files import, by Qt's scanner.
"$qt/libexec/qmlimportscanner" -rootPath /src/app -importPath "$qt/qml" > /tmp/imports.json
python3 - "$qt/qml" "$out/qml" <<'PY'
import json, os, shutil, sys
src_root, dst_root = sys.argv[1], sys.argv[2]
seen = set()
for entry in json.load(open('/tmp/imports.json')):
    path = entry.get('path')
    if entry.get('type') != 'module' or not path or not path.startswith(src_root) or path in seen:
        continue
    seen.add(path)
    dst = os.path.join(dst_root, os.path.relpath(path, src_root))
    if not os.path.exists(dst):
        shutil.copytree(path, dst, symlinks=True, dirs_exist_ok=True)
print(f"qml modules: {len(seen)}")
PY

# Qt's libraries, for the client and for everything copied above, until the
# set stops growing. Only libraries under $qt/lib are taken.
todo=$(mktemp)
{ echo "$out/bin/OmnuvClient.bin"; find "$out/plugins" "$out/qml" -name '*.so*' -type f; } > "$todo"
while :; do
    before=$(ls "$out/lib" | wc -l)
    while read -r f; do
        LD_LIBRARY_PATH="$qt/lib" ldd "$f" 2>/dev/null | awk '/=>/ {print $3}' | while read -r lib; do
            case "$lib" in "$qt"/lib/*) cp -Ln "$lib" "$out/lib/" 2>/dev/null || true ;; esac
        done
    done < "$todo"
    after=$(ls "$out/lib" | wc -l)
    [ "$after" -eq "$before" ] && break
    find "$out/lib" -name '*.so*' -type f > "$todo"
done
rm -f "$todo"

cat > "$out/bin/qt.conf" <<'CONF'
[Paths]
Prefix = ..
Libraries = lib
Plugins = plugins
QmlImports = qml
CONF

# **Proof, not hope: the bundle alone, with the build's Qt out of reach.**
# Every library the client needs must now resolve from the bundle or the
# system; one that still points into $qt was missed.
missing=$(LD_LIBRARY_PATH="$out/lib" ldd "$out/bin/OmnuvClient.bin" | grep -E "not found|$qt" || true)
[ -z "$missing" ] || { echo "the bundle does not stand alone:" >&2; echo "$missing" >&2; exit 1; }
echo "STAGED=$out libs=$(ls "$out/lib" | wc -l) plugins=$(find "$out/plugins" -name '*.so' | wc -l) size=$(du -sh "$out" | cut -f1)"
