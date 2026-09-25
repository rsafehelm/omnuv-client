#!/bin/sh
# Who the macOS postinstall records as the tunnel's owner, in a scratch root,
# with `stat` stubbed to answer as macOS's does for /dev/console.
# Run: sh packaging/connect/macos/postinstall_test.sh (exit 0 = all pass).
set -u
here=$(cd "$(dirname "$0")" && pwd)
fail=0
case_() { # description, expected owner, console uid ("" = nobody), env...
    what=$1; want=$2; console=$3; shift 3
    root=$(mktemp -d); bin=$(mktemp -d)
    printf '#!/bin/sh\n[ -n "%s" ] && echo %s\n' "$console" "$console" > "$bin/stat"; chmod +x "$bin/stat"
    if [ -n "${EXISTING:-}" ]; then mkdir -p "$root/var/lib/onv/tunnel"; echo "$EXISTING" > "$root/var/lib/onv/tunnel/owner"; fi
    env -u SUDO_UID PATH="$bin:$PATH" ONV_ROOT="$root" "$@" sh "$here/postinstall" >/dev/null
    got=$(cat "$root/var/lib/onv/tunnel/owner" 2>/dev/null || true)
    if [ "$got" = "$want" ]; then echo "ok    $what"; else echo "FAIL  $what: owner '$got', wanted '$want'"; fail=1; fi
    rm -rf "$root" "$bin"
}
case_ "the Installer app: the console's person owns it" 501 501
case_ "sudo installer: that person, not the console" 502 501 SUDO_UID=502
case_ "root at the console: nobody yet" "" 0
case_ "nobody at the console: nobody yet" "" ""
EXISTING=503 case_ "an upgrade keeps the owner it has" 503 501
exit $fail
