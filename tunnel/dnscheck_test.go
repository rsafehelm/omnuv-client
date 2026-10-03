package main

import (
	"context"
	"errors"
	"net"
	"testing"
)

// A resolver with a cache in front of the truth: lookups answer the cache
// until flush copies the truth into it. `stuck` names keep their cached
// answer through a flush, as a stale answer from somewhere other than the
// system cache would.
type fakeResolver struct {
	truth, cache map[string]string // "" = no such host
	stuck        map[string]bool
	timeout      map[string]bool
	flushes      int
}

func (r *fakeResolver) world(peers map[string]string) dnsWorld {
	return dnsWorld{
		peers: func() map[string]string { return peers },
		lookup: func(_ context.Context, host string) ([]string, error) {
			if r.timeout[host] {
				return nil, &net.DNSError{Name: host, IsTimeout: true}
			}
			if ip := r.cache[host]; ip != "" {
				return []string{ip}, nil
			}
			return nil, &net.DNSError{Name: host, Err: "no such host", IsNotFound: true}
		},
		flush: func() error {
			r.flushes++
			for k, v := range r.truth {
				if !r.stuck[k] {
					r.cache[k] = v
				}
			}
			return nil
		},
	}
}

func TestAStaleNameIsFlushedAndThenResolvesToTheOverlay(t *testing.T) {
	// The operator's case of 3 October 2026: the machine was re-made under
	// its old name and the system still answered its old address.
	r := &fakeResolver{
		truth: map[string]string{"ollama-53746c3e.internal": "10.210.90.192", "hermes.internal": "10.211.70.124"},
		cache: map[string]string{"ollama-53746c3e.internal": "10.212.172.225", "hermes.internal": "10.211.70.124"},
	}
	still := checkNames(context.Background(), r.world(r.truth))
	if r.flushes != 1 {
		t.Fatalf("flushed %d times, want 1", r.flushes)
	}
	if len(still) != 0 {
		t.Fatalf("still stale after the flush: %v", still)
	}
}

func TestANameCachedAsMissingIsStaleToo(t *testing.T) {
	// Asked for before the machine existed: a cached "no such host".
	r := &fakeResolver{
		truth: map[string]string{"chat-2bd4d7fd.internal": "10.210.1.2"},
		cache: map[string]string{},
	}
	checkNames(context.Background(), r.world(r.truth))
	if r.flushes != 1 || r.cache["chat-2bd4d7fd.internal"] != "10.210.1.2" {
		t.Fatalf("flushes %d, cache %v", r.flushes, r.cache)
	}
}

func TestAStaleAnswerTheFlushDoesNotChangeIsReported(t *testing.T) {
	r := &fakeResolver{
		truth: map[string]string{"x.internal": "10.210.0.2"},
		cache: map[string]string{"x.internal": "10.210.0.9"},
		stuck: map[string]bool{"x.internal": true},
	}
	still := checkNames(context.Background(), r.world(r.truth))
	if got := still["x.internal"]; len(got) != 1 || got[0] != "10.210.0.9" {
		t.Fatalf("want x.internal still stale at 10.210.0.9, got %v", still)
	}
}

func TestNothingIsFlushedWhenEveryNameIsRightOrCouldNotBeAsked(t *testing.T) {
	// "Could not ask" is not "stale": a timeout decides nothing.
	r := &fakeResolver{
		truth:   map[string]string{"a.internal": "10.210.0.1", "b.internal": "10.210.0.2"},
		cache:   map[string]string{"a.internal": "10.210.0.1"},
		timeout: map[string]bool{"b.internal": true},
	}
	checkNames(context.Background(), r.world(r.truth))
	if r.flushes != 0 {
		t.Fatalf("flushed %d times with nothing stale", r.flushes)
	}
	// And with the tunnel down there is no map to hold anything against.
	checkNames(context.Background(), r.world(nil))
	if r.flushes != 0 {
		t.Fatalf("flushed with no overlay map")
	}
}

func TestAFailedFlushIsReportedAndNotRetriedInTheSamePass(t *testing.T) {
	r := &fakeResolver{
		truth: map[string]string{"x.internal": "10.210.0.2"},
		cache: map[string]string{"x.internal": "10.210.0.9"},
	}
	w := r.world(r.truth)
	calls := 0
	w.flush = func() error { calls++; return errors.New("denied") }
	if still := checkNames(context.Background(), w); len(still) != 1 || calls != 1 {
		t.Fatalf("calls %d, still %v", calls, still)
	}
}

// The real tunnel exposes no map unless it runs.
func TestTheTunnelHasNoPeersUnlessRunning(t *testing.T) {
	tn := &tunnel{state: stateStopped, client: &fakeClient{}}
	if p := tn.peers(); p != nil {
		t.Fatalf("peers while stopped: %v", p)
	}
}
