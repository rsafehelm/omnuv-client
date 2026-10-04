package main

import (
	"encoding/json"
	"strings"
	"testing"
)

type pathsClient struct {
	fakeClient
	paths []peerPath
}

func (p *pathsClient) PeerPaths() []peerPath { return p.paths }

// **The path is found by the peer's address, and only that peer answers.**
// A near address (one more digit) and every other peer stay out, and a tunnel
// that is not running answers its state with no peers rather than an error,
// so a reader can tell "stopped" from "no such peer".
func TestPeersV1(t *testing.T) {
	tn := &tunnel{state: stateStopped, dir: t.TempDir()}
	read := func(line string) peersView {
		t.Helper()
		got := answer(tn, line)
		var view peersView
		if err := json.Unmarshal([]byte(got), &view); err != nil {
			t.Fatalf("%q answered %q, not JSON: %v", line, got, err)
		}
		return view
	}

	if v := read("peers-v1"); v.State != stateStopped || v.Peers == nil || len(v.Peers) != 0 {
		t.Fatalf("a stopped tunnel answers its state and an empty list, got %+v", v)
	}

	tn.client = &pathsClient{paths: []peerPath{
		{IP: "100.92.3.4", PubKey: "ours=", Status: "Connected", Relayed: true, RelayServer: "rels://relay.example:443",
			LocalIceType: "host", RemoteIceType: "srflx", LatencyMs: 21.5},
		{IP: "100.92.3.40", PubKey: "near=", Status: "Connected"},
		{IP: "100.92.9.9", PubKey: "other=", Status: "Idle"},
	}}
	// A client left from a failed start is not a running tunnel: what its
	// recorder holds is stale, and nothing of it is answered.
	tn.state = stateFailed
	if v := read("peers-v1"); v.State != stateFailed || len(v.Peers) != 0 {
		t.Fatalf("a tunnel that is not running answers no peers, got %+v", v)
	}
	tn.state = stateRunning

	if v := read("peers-v1"); v.State != stateRunning || len(v.Peers) != 3 {
		t.Fatalf("with no address every peer answers, got %+v", v)
	}
	v := read("peers-v1 100.92.3.4")
	if len(v.Peers) != 1 || v.Peers[0].PubKey != "ours=" || !v.Peers[0].Relayed ||
		v.Peers[0].RelayServer != "rels://relay.example:443" || v.Peers[0].LatencyMs != 21.5 {
		t.Fatalf("the peer at the address, and only it, got %+v", v)
	}
	if v := read("peers-v1 100.92.3.5"); len(v.Peers) != 0 {
		t.Fatalf("an address no peer holds answers none, got %+v", v)
	}
	for _, bad := range []string{"peers-v1 steam-1a2b3c4d", "peers-v1 100.92.3.4 extra"} {
		if got := answer(tn, bad); !strings.HasPrefix(got, "err ") {
			t.Fatalf("%q must be refused: a peer is asked for by address, got %q", bad, got)
		}
	}
}

// The paths are the owner's, like everything but `state`, and asking for
// them claims nothing: it is a read.
func TestPeersV1IsTheOwners(t *testing.T) {
	tn := &tunnel{state: stateStopped, dir: t.TempDir()}
	alice, bob := caller{id: "S-1-5-21-alice"}, caller{id: "S-1-5-21-bob"}
	if got := answerFor(tn, alice, "peers-v1"); strings.HasPrefix(got, "err") {
		t.Fatalf("an unowned tunnel answers anyone, got %q", got)
	}
	tn.mu.Lock()
	owner, err := tn.ownerLocked()
	tn.mu.Unlock()
	if err != nil || owner != "" {
		t.Fatalf("a read must not claim the tunnel, owner %q err %v", owner, err)
	}
	if err := tn.claim(alice); err != nil {
		t.Fatal(err)
	}
	if got := answerFor(tn, bob, "peers-v1"); !strings.HasPrefix(got, "err not-owner") {
		t.Fatalf("another user is refused, got %q", got)
	}
	if got := answerFor(tn, caller{admin: true}, "peers-v1"); strings.HasPrefix(got, "err") {
		t.Fatalf("an administrator may read, got %q", got)
	}
}
