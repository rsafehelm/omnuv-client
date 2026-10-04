#!/bin/sh
# Prints the digest of the tunnel's source, one line of hex.
#
#     tunnel/source-digest.sh
#
# **What a daemon was built from, in a form the shipper can recompute**
# (4 October 2026). build.sh records this beside each daemon it writes to
# dist/ (`<daemon>.BUILT_FROM`, `source=`), and every play that ships a daemon
# from dist/ runs this script on the tree it is shipping and refuses a daemon
# whose record differs. Until then nothing asked: lab-windows-build.yml sent
# the 3 October daemons, without `peers-v1`, beside client 7f0f3f5e, and only
# reading the files' dates caught it.
#
# **The files are the ones a rig play ships**: tracked, or untracked and not
# ignored, under tunnel/, dist/ excluded, as they are on disk now, so
# uncommitted work counts. Each is hashed with its path from the repository
# root, and the sorted list is hashed. A clean tree and its commit give the
# same digest on any machine; build.sh also records the commit, the git tree
# of tunnel/ and whether it was dirty, for a person to read.
#
# POSIX sh, and only what BusyBox has: the Ansible container is Alpine, and
# its digest must equal the one a GNU build host wrote.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
g() { git -c safe.directory='*' -C "$root" "$@"; }
g rev-parse --git-dir >/dev/null 2>&1 || {
    echo "source-digest.sh: $root is not a git work tree this git can read" >&2
    exit 2
}
tracked=$(g ls-files -- tunnel)
untracked=$(g ls-files --others --exclude-standard -- tunnel)
list=$(printf '%s\n%s\n' "$tracked" "$untracked" | grep -v -e '^tunnel/dist/' -e '^$' | LC_ALL=C sort -u)
[ -n "$list" ] || { echo "source-digest.sh: no tunnel source under $root/tunnel" >&2; exit 2; }
cd "$root"
printf '%s\n' "$list" | while IFS= read -r f; do
    if [ -f "$f" ]; then sha256sum "$f"; fi
done | sha256sum | cut -d' ' -f1
