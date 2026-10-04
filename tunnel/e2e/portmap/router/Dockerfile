# A home router, as far as port mapping goes: miniupnpd 2.3.9 (UPnP-IGD v1
# and v2, NAT-PMP, PCP), with the nftables backend. Pinned by digest and by
# package version, so a run means the same router every time.
#
# Debian rather than Alpine: Alpine 3.22 ships miniupnpd only with the
# legacy iptables backend, which needs the ip_tables kernel module, and this
# host runs nftables (measured 4 October 2026: "iptc_init() failed : iptables
# who?"). Loading a module on the host is a host change; this is not.
FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a
RUN apt-get -qq update \
 && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
      miniupnpd-nftables=2.3.9-2+deb13u1 nftables iproute2 > /dev/null \
 && rm -rf /var/lib/apt/lists/*
COPY entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/bin/sh", "/entrypoint.sh"]
