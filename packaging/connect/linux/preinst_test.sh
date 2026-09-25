#!/bin/sh
# The preinst's three answers, with a stub dpkg-query and a scratch root.
# Run: sh packaging/connect/linux/preinst_test.sh (exit 0 = all pass).
set -u
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d); trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/bin" "$scratch/root/etc/apt/sources.list.d"
stub() { printf '#!/bin/sh\n%s\n' "$1" > "$scratch/bin/dpkg-query"; chmod +x "$scratch/bin/dpkg-query"; }
run() { PATH="$scratch/bin:$PATH" ONV_ROOT="$scratch/root" sh "$here/preinst" install 2>"$scratch/err"; }
fail=0
check() { if [ "$1" = "$2" ]; then echo "ok    $3"; else echo "FAIL  $3 (exit $2, wanted $1)"; fail=1; fi; }
said() { if grep -q "$1" "$scratch/err"; then echo "ok    $2"; else echo "FAIL  $2: $(cat "$scratch/err")"; fail=1; fi; }

stub 'exit 1'                                  # netbird not installed at all
run; check 0 $? "no netbird: installs"

stub 'printf "install ok installed"'           # a person's own netbird
run; check 1 $? "their own netbird: refused"
said "Remove netbird first" "their own netbird: told to remove it, not removed"

stub 'printf "hold ok installed"'              # an Omnuv image: held, and ours
touch "$scratch/root/etc/apt/sources.list.d/onv-netbird.list"
run; check 1 $? "an Omnuv machine: refused"
said "a machine you rent from Omnuv" "an Omnuv machine: told Connect is for their own devices"

stub 'printf "deinstall ok config-files"'      # removed, config left: not installed
run; check 0 $? "netbird removed but configured: installs"
exit $fail
