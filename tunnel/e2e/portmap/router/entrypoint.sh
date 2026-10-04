#!/bin/sh
# The router: `entrypoint.sh on <lan prefix> <wan prefix>` runs miniupnpd;
# `off` runs nothing, so the machine is a gateway with no mapping service.
#
# What it holds is written to /state by the router itself, never by the
# client under test: miniupnpd's lease file (`lease_file`), and the nftables
# table miniupnpd writes its redirects into, dumped every half second. The
# test reads those to decide whether a mapping is on the router, which is the
# fact, rather than trusting what the client library says it did.
set -eu
mode="$1" lan="$2" wan="$3"
iface() { ip -o -4 addr show | awk -v p="$1" 'index($4, p) == 1 { print $2; exit }'; }
lan_if=$(iface "$lan")
wan_if=$(iface "$wan")
[ -n "$lan_if" ] && [ -n "$wan_if" ] || { echo "router: no interface on $lan or $wan" >&2; ip -o -4 addr show >&2; exit 1; }
mkdir -p /state
: > /state/upnp.leases
echo "router: lan $lan_if, wan $wan_if, mode $mode"
if [ "$mode" = off ]; then
    echo "router: no port mapping service" > /state/ready
    exec sleep 3600
fi
# The WAN side is a private 10.x network, as it is behind a carrier NAT;
# miniupnpd refuses that unless told (it would otherwise ask STUN).
cat > /etc/miniupnpd/miniupnpd.conf <<EOF
ext_ifname=$wan_if
listening_ip=$lan_if
ext_allow_private_ipv4=yes
http_port=5000
enable_pcp_pmp=yes
enable_upnp=yes
lease_file=/state/upnp.leases
secure_mode=yes
system_uptime=yes
min_lifetime=120
max_lifetime=86400
uuid=6f2e3c1a-0b7d-4e7a-9c1e-0a1b2c3d4e5f
allow 1024-65535 ${lan}0/24 1024-65535
deny 0-65535 0.0.0.0/0 0-65535
EOF
/etc/miniupnpd/nft_init.sh > /state/init.log 2>&1
(
    while :; do
        nft list ruleset > /state/nat.tmp 2>&1 && mv /state/nat.tmp /state/nat.rules
        sleep 0.5 # wait: a dump, not a poll; the reader polls the file
    done
) &
echo "router: miniupnpd" > /state/ready
exec miniupnpd -d -f /etc/miniupnpd/miniupnpd.conf
