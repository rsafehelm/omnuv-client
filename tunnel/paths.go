package main

import (
	"encoding/json"
	"net/netip"
	"strings"
	"time"
)

// The path to each peer, as NetBird's status recorder holds it: `peers-v1`.
//
// **Why the tunnel says this rather than a log.** A stream that loses frames
// over the overlay is a different finding when the path is relayed than when
// it is direct, and which it was is a fact the library holds in memory
// (`Client.Status`) and nothing outside this process can ask. Before this the
// only witnesses were NetBird's log lines and the WireGuard endpoint, both
// side effects (omnuv: fixed.md, "Gaming runs fail the stream gate on loss",
// 4 October 2026).
//
// A peer is reported with its overlay address and WireGuard key, the ids it
// is found by; its DNS name is carried for a person to read and never used to
// find it. Endpoints and relay addresses are underlay facts of this machine's
// own network, which is why the answer is the owner's, like everything but
// `state` (owner.go).
type peerPath struct {
	IP                string    `json:"ip"`
	PubKey            string    `json:"pubkey"`
	FQDN              string    `json:"fqdn,omitempty"`
	Status            string    `json:"status"`
	StatusSince       time.Time `json:"status_since"`
	Relayed           bool      `json:"relayed"`
	RelayServer       string    `json:"relay_server,omitempty"`
	LocalIceType      string    `json:"local_ice_type,omitempty"`
	RemoteIceType     string    `json:"remote_ice_type,omitempty"`
	LocalIceEndpoint  string    `json:"local_ice_endpoint,omitempty"`
	RemoteIceEndpoint string    `json:"remote_ice_endpoint,omitempty"`
	LatencyMs         float64   `json:"latency_ms"`
	LastHandshake     time.Time `json:"last_handshake"`
	BytesRx           int64     `json:"bytes_rx"`
	BytesTx           int64     `json:"bytes_tx"`
}

// The daemon's state beside the peers, so "no peers" from a stopped tunnel
// and "no peers" from a running one read differently.
type peersView struct {
	State int        `json:"state"`
	Peers []peerPath `json:"peers"`
}

func (c *embeddedClient) PeerPaths() []peerPath {
	status, err := c.Status()
	if err != nil {
		return nil
	}
	out := make([]peerPath, 0, len(status.Peers))
	for _, p := range status.Peers {
		ip, _, _ := strings.Cut(p.IP, "/")
		out = append(out, peerPath{
			IP:                ip,
			PubKey:            p.PubKey,
			FQDN:              strings.TrimSuffix(p.FQDN, "."),
			Status:            p.ConnStatus.String(),
			StatusSince:       p.ConnStatusUpdate.UTC(),
			Relayed:           p.Relayed,
			RelayServer:       p.RelayServerAddress,
			LocalIceType:      p.LocalIceCandidateType,
			RemoteIceType:     p.RemoteIceCandidateType,
			LocalIceEndpoint:  p.LocalIceCandidateEndpoint,
			RemoteIceEndpoint: p.RemoteIceCandidateEndpoint,
			LatencyMs:         float64(p.Latency) / float64(time.Millisecond),
			LastHandshake:     p.LastWireguardHandshake.UTC(),
			BytesRx:           p.BytesRx,
			BytesTx:           p.BytesTx,
		})
	}
	return out
}

// Every peer while the tunnel runs, or the one at `address`; none otherwise.
func (t *tunnel) peerPaths(address string) (string, error) {
	var want netip.Addr
	if address != "" {
		a, err := netip.ParseAddr(address)
		if err != nil {
			return "", err
		}
		want = a
	}
	t.mu.Lock()
	c, state := t.client, t.state
	t.mu.Unlock()
	view := peersView{State: state, Peers: []peerPath{}}
	if c != nil && state == stateRunning {
		for _, p := range c.PeerPaths() {
			if want.IsValid() {
				if got, err := netip.ParseAddr(p.IP); err != nil || got != want {
					continue
				}
			}
			view.Peers = append(view.Peers, p)
		}
	}
	raw, err := json.Marshal(view)
	if err != nil {
		return "", err
	}
	return string(raw), nil
}
