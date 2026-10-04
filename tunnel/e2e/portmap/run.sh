#!/usr/bin/env bash
# NetBird's port mapper, and the tunnel's watch over it, against a router
# emulator: tunnel/e2e/portmap/run.sh [mode ...]
#
#     modes   upnp natpmp pcp none disabled   (all, by default)
#
# **Nothing real is touched.** The router is miniupnpd in a container this
# script starts, on two Docker networks it makes for the run (explicit
# 10.x subnets, never 172.31.x, both --internal), named onvt-pm-<run>-…, and
# removed at the end, success or not. It refuses to start over an earlier
# run's leftovers and names the command that removes them.
#
# The client is a golang container on the router's LAN with its default
# route through the router, running NetBird's own portforward package with
# one test laid into it (portforward_e2e_test.go.in), beside the tunnel's
# portmap_observe.go and portmap_release.go, renamed into that package. Go
# refuses an overlay beneath the module cache, so the run copies NetBird
# v0.78.1 out of the cache and points a copy of the tunnel's go.mod at it
# with a `replace`: the same source, byte for byte, plus three files. Each mode prints PROOF lines; the verdict is go test's exit
# status, per mode.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
tunnel="$(cd "$here/../.." && pwd)"
modes="${*:-upnp natpmp pcp none disabled}"
run="$(date -u +%Y%m%dT%H%M%S)-$$"
prefix="onvt-pm-$run"
out="${ONV_E2E_OUT:-$(mktemp -d)}"; mkdir -p "$out"
GO_IMAGE=golang:1.26-alpine
ROUTER_IMAGE=onvt-pm-router:2.3.7
NB_MOD=/go/pkg/mod/github.com/netbirdio/netbird@v0.78.1

left="$(docker ps -a --format '{{.Names}}' | grep '^onvt-pm-' || true; docker network ls --format '{{.Name}}' | grep '^onvt-pm-' || true)"
if [ -n "$left" ]; then
    echo "run.sh: an earlier run left these behind:" >&2
    echo "$left" >&2
    echo "remove them with: docker rm -f \$(docker ps -aq --filter name=^onvt-pm-); docker network rm \$(docker network ls -q --filter name=^onvt-pm-)" >&2
    exit 3
fi

cleanup() {
    docker ps -aq --filter "name=^$prefix-" | xargs -r docker rm -f > /dev/null
    docker network ls -q --filter "name=^$prefix-" | xargs -r docker network rm > /dev/null
    rest="$(docker ps -a --format '{{.Names}}' | grep "^$prefix-" || true; docker network ls --format '{{.Name}}' | grep "^$prefix-" || true)"
    if [ -n "$rest" ]; then echo "run.sh: LEFT BEHIND: $rest" >&2; else echo "cleanup: nothing of $prefix remains"; fi
}
trap cleanup EXIT

# Two /24s in 10.128-254, neither overlapping any network Docker has.
taken="$(docker network inspect $(docker network ls -q) --format '{{range .IPAM.Config}}{{.Subnet}} {{end}}' | tr ' ' '\n' | grep -v '^$' || true)"
pick() {
    local a b n
    for _ in $(seq 50); do
        a=$((128 + RANDOM % 127)); b=$((RANDOM % 254))
        n="10.$a.$b.0/24"
        if ! printf '%s\n' "$taken" | python3 -c "
import ipaddress, sys
want = ipaddress.ip_network('$n')
sys.exit(any(want.overlaps(ipaddress.ip_network(l.strip(), strict=False)) for l in sys.stdin if l.strip()))"; then
            continue
        fi
        taken="$taken
$n"; echo "10.$a.$b."; return
    done
    echo "run.sh: no free 10.x /24" >&2; exit 1
}
# pick runs in a subshell, so what it chose is added to taken here.
lan="$(pick)"; taken="$taken
${lan}0/24"; wan="$(pick)"
docker network create --internal --subnet "${lan}0/24" "$prefix-lan" > /dev/null
docker network create --internal --subnet "${wan}0/24" "$prefix-wan" > /dev/null
echo "networks: $prefix-lan ${lan}0/24, $prefix-wan ${wan}0/24"

docker build -q -t "$ROUTER_IMAGE" "$here/router" > /dev/null

# The package under test, with the tunnel's two files moved into it.
src="$out/src"
if [ -d "$src" ]; then docker run --rm -v "$out:/out" alpine:3.22 rm -rf /out/src; fi
mkdir -p "$src"
cp -a "$tunnel/." "$src/tunnel/"; rm -rf "$src/tunnel/dist" "$src/tunnel/e2e"
docker run --rm -v omnuv_go_mod:/go/pkg/mod:ro -v "$src:/src" alpine:3.22 \
    sh -ec "cp -a $NB_MOD /src/nb && chmod -R u+w /src/nb && chown -R $(id -u):$(id -g) /src/nb"
pf="$src/nb/client/internal/portforward"
for f in portmap_observe.go portmap_release.go; do
    sed '1s/^package main$/package portforward/' "$tunnel/$f" > "$pf/onv_$f"
done
cp "$here/portforward_e2e_test.go.in" "$pf/onv_e2e_test.go"
printf '\nreplace github.com/netbirdio/netbird v0.78.1 => /nb\n' >> "$src/tunnel/go.mod"

fail=0
for mode in $modes; do
    router="$prefix-router-$mode"
    service=on; [ "$mode" = none ] && service=off
    rm -rf "$out/state-$mode"; mkdir -p "$out/state-$mode"
    docker create --name "$router" --cap-add NET_ADMIN --network "$prefix-lan" --ip "${lan}2" \
        -v "$out/state-$mode:/state" "$ROUTER_IMAGE" "$service" "$lan" "$wan" > /dev/null
    docker network connect --ip "${wan}2" "$prefix-wan" "$router"
    docker start "$router" > /dev/null
    for _ in $(seq 50); do [ -f "$out/state-$mode/ready" ] && break; sleep 0.2; done
    [ -f "$out/state-$mode/ready" ] || { echo "router did not come up"; docker logs "$router"; exit 1; }
    set +e
    docker run --rm --name "$prefix-client-$mode" --cap-add NET_ADMIN --network "$prefix-lan" --ip "${lan}10" \
        -e GOPROXY=off -e GOFLAGS=-mod=mod -e GONOSUMDB='*' -e GOTOOLCHAIN=local -e ONV_E2E_MODE="$mode" \
        -v "$src/tunnel:/w" -v "$src/nb:/nb" -v "$out/state-$mode:/state:ro" \
        -v omnuv_go_mod:/go/pkg/mod -v onvt_pm_go_build:/root/.cache/go-build -w /w "$GO_IMAGE" \
        sh -ec "ip route replace default via ${lan}2 && go test -count=1 -v -run TestOnvRouterEmulator github.com/netbirdio/netbird/client/internal/portforward" \
        > "$out/$mode.log" 2>&1
    rc=$?
    set -e
    docker logs "$router" > "$out/router-$mode.log" 2>&1 || true
    docker rm -f "$router" > /dev/null
    if [ $rc -eq 0 ]; then echo "ok      $mode"; else echo "FAILED  $mode (exit $rc)"; fail=1; fi
    grep -E 'PROOF|--- FAIL|portmap_test|_test.go:[0-9]+:' "$out/$mode.log" | sed 's/^/        /' || true
done
echo "logs: $out"
exit $fail
