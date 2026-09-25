#!/bin/sh
# Who the postinst records as the tunnel's owner, in a scratch root.
# Run: sh packaging/connect/linux/postinst_test.sh (exit 0 = all pass).
set -u
here=$(cd "$(dirname "$0")" && pwd)
fail=0
case_() { # description, expected owner ("" = none), env..., then the existing owner if any
    what=$1; want=$2; shift 2
    root=$(mktemp -d)
    if [ -n "${EXISTING:-}" ]; then mkdir -p "$root/var/lib/onv/tunnel"; echo "$EXISTING" > "$root/var/lib/onv/tunnel/owner"; fi
    env -u SUDO_UID -u PKEXEC_UID ONV_ROOT="$root" "$@" sh "$here/postinst" configure >/dev/null
    got=$(cat "$root/var/lib/onv/tunnel/owner" 2>/dev/null || true)
    if [ "$got" = "$want" ]; then echo "ok    $what"; else echo "FAIL  $what: owner '$got', wanted '$want'"; fail=1; fi
    if [ -n "$got" ] && [ -z "${EXISTING:-}" ]; then
        mode=$(stat -c %a "$root/var/lib/onv/tunnel/owner")
        [ "$mode" = 600 ] || { echo "FAIL  $what: owner file is mode $mode, wanted 600"; fail=1; }
    fi
    rm -rf "$root"
}
case_ "installed with sudo: that person owns it" 1000 SUDO_UID=1000
case_ "installed with pkexec: that person owns it" 1001 PKEXEC_UID=1001
case_ "installed by root itself: nobody yet" "" SUDO_UID=0
case_ "installed by a service: nobody yet" ""
EXISTING=1002 case_ "an upgrade keeps the owner it has" 1002 SUDO_UID=1000
case_ "a uid that is not a number is not written" "" SUDO_UID='1000; rm -rf /'
exit $fail
