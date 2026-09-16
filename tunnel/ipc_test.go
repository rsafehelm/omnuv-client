package main

import "testing"

// The protocol's refusals, which are the half that matters: every one of these
// is a case where answering wrongly costs something real — a peer nobody asked
// for, a device on a stranger's network, or a state that reads as "off the
// network" when it means "nobody looked".
func TestAnswer(t *testing.T) {
	tn := &tunnel{state: stateStopped}

	if got := answer(tn, "state"); got != "state 0 " {
		t.Fatalf("a fresh daemon should report stopped with no reason, got %q", got)
	}
	if got := answer(tn, ""); got[:3] != "err" {
		t.Fatalf("an empty request is an error, got %q", got)
	}
	if got := answer(tn, "wibble"); got[:3] != "err" {
		t.Fatalf("an unknown request is an error, got %q", got)
	}

	// A key with no address, and an address with no key: both refused, because
	// the library would otherwise fall back to the vendor's own cloud and
	// enrol there — a device on somebody else's network, which is worse than a
	// failure because it looks like success.
	if got := answer(tn, "enrol somekey"); got[:3] != "err" {
		t.Fatalf("enrol needs both a url and a key, got %q", got)
	}
	if got := answer(tn, "enrol https://example.invalid key extra"); got[:3] != "err" {
		t.Fatalf("enrol takes exactly two arguments, got %q", got)
	}

	// **A resume on a machine that has never enrolled must refuse**, not
	// enrol. This is the one that would have cost a peer per machine per
	// start, and the daemon resumes at boot, so it is also the one that would
	// have done it unattended.
	got := answer(tn, "resume")
	if got[:13] != "err need-key " {
		t.Fatalf("a resume with no identity must refuse as need-key, got %q", got)
	}
	if state, why := tn.snapshot(); state != stateStopped || why == "" {
		t.Fatalf("a refused resume leaves it stopped with a reason, got %d %q", state, why)
	}

	// And `stop` with nothing running is not an error: the caller asked for a
	// state, it is in that state.
	if got := answer(tn, "stop"); got != "ok" {
		t.Fatalf("stopping a stopped tunnel is fine, got %q", got)
	}
}
