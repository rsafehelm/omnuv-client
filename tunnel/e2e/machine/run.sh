#!/usr/bin/env bash
# Machine mode against a real NetBird: tunnel/e2e/machine/run.sh
#
# **Nothing real is touched.** NetBird v0.78.1's own management and signal
# servers run inside the test process, built as NetBird's client/embed test
# builds them, with a SQLite store from its own testdata; the tunnel's real
# start path makes a WireGuard interface inside a container. The container
# sits on a Docker network this script makes for the run (an explicit 10.x
# subnet, never 172.31.x, --internal), named onvt-mm-<run>-…, removed at the
# end, success or not. It refuses to start over an earlier run's leftovers and
# names the command that removes them.
#
# Two containers: one builds the test binary (it may fetch the management
# server's modules into the omnuv_go_mod cache the first time), the other
# runs it, with NET_ADMIN and /dev/net/tun, offline. The test is
# machine_e2e_test.go.in, laid into a copy of the tunnel's package; the
# verdict is its exit status, and each fact it proved is a PROOF line.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
tunnel="$(cd "$here/../.." && pwd)"
run="$(date -u +%Y%m%dT%H%M%S)-$$"
prefix="onvt-mm-$run"
out="${ONV_E2E_OUT:-$(mktemp -d)}"; mkdir -p "$out"
GO_IMAGE=golang:1.26
NB_MOD=/go/pkg/mod/github.com/netbirdio/netbird@v0.78.1

left="$(docker ps -a --format '{{.Names}}' | grep '^onvt-mm-' || true; docker network ls --format '{{.Name}}' | grep '^onvt-mm-' || true)"
if [ -n "$left" ]; then
    echo "run.sh: an earlier run left these behind:" >&2
    echo "$left" >&2
    echo "remove them with: docker rm -f \$(docker ps -aq --filter name=^onvt-mm-); docker network rm \$(docker network ls -q --filter name=^onvt-mm-)" >&2
    exit 3
fi

cleanup() {
    docker ps -aq --filter "name=^$prefix-" | xargs -r docker rm -f > /dev/null
    docker network ls -q --filter "name=^$prefix-" | xargs -r docker network rm > /dev/null
    rest="$(docker ps -a --format '{{.Names}}' | grep "^$prefix-" || true; docker network ls --format '{{.Name}}' | grep "^$prefix-" || true)"
    if [ -n "$rest" ]; then echo "run.sh: LEFT BEHIND: $rest" >&2; else echo "cleanup: nothing of $prefix remains"; fi
}
trap cleanup EXIT

# One /24 in 10.128-254, overlapping no network Docker has.
taken="$(docker network inspect $(docker network ls -q) --format '{{range .IPAM.Config}}{{.Subnet}} {{end}}' | tr ' ' '\n' | grep -v '^$' || true)"
subnet=""
for _ in $(seq 50); do
    n="10.$((128 + RANDOM % 127)).$((RANDOM % 254)).0/24"
    if printf '%s\n' "$taken" | python3 -c "
import ipaddress, sys
want = ipaddress.ip_network('$n')
sys.exit(any(want.overlaps(ipaddress.ip_network(l.strip(), strict=False)) for l in sys.stdin if l.strip()))"; then
        subnet="$n"; break
    fi
done
[ -n "$subnet" ] || { echo "run.sh: no free 10.x /24" >&2; exit 1; }
docker network create --internal --subnet "$subnet" "$prefix-net" > /dev/null
echo "network: $prefix-net $subnet"

src="$out/src"
if [ -d "$src" ]; then docker run --rm -v "$out:/out" alpine:3.22 rm -rf /out/src /out/machine.test; fi
mkdir -p "$src"
cp -a "$tunnel/." "$src/tunnel/"; rm -rf "$src/tunnel/dist" "$src/tunnel/e2e"
cp "$here/machine_e2e_test.go.in" "$src/tunnel/machine_e2e_test.go"

echo "build: the tunnel's package with the e2e test, against NetBird's management server"
# As root, because the module cache is root's; what it wrote is handed back.
docker run --rm --name "$prefix-build" -e GOFLAGS=-mod=mod -e CGO_ENABLED=1 \
    -v onvt_mm_go_build:/root/.cache/go-build \
    -v omnuv_go_mod:/go/pkg/mod -v "$src/tunnel:/w" -v "$out:/out" -w /w "$GO_IMAGE" \
    bash -ec "trap 'chown -R $(id -u):$(id -g) /out' EXIT; go mod tidy > /out/tidy.log 2>&1 && go test -c -o /out/machine.test ." > "$out/build.log" 2>&1 \
    || { echo "FAILED  build"; tail -30 "$out/build.log" "$out/tidy.log" 2>/dev/null; exit 1; }

set +e
docker run --rm --name "$prefix-machine" --network "$prefix-net" --cap-add NET_ADMIN --device /dev/net/tun \
    -e ONV_E2E_STORE_SQL="$NB_MOD/client/testdata/store.sql" \
    -v omnuv_go_mod:/go/pkg/mod:ro -v "$out:/out:ro" "$GO_IMAGE" \
    /out/machine.test -test.v -test.count=1 -test.run TestOnvMachineModeAgainstNetBird > "$out/machine.log" 2>&1
rc=$?
set -e
grep -E 'PROOF|--- (PASS|FAIL)|_test.go:[0-9]+:' "$out/machine.log" | grep -v '^\s*$' | sed 's/^/        /' || true
if [ $rc -eq 0 ]; then echo "ok      machine mode"; else echo "FAILED  machine mode (exit $rc)"; fi
echo "logs: $out"
exit $rc
