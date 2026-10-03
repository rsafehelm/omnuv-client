package main

import (
	"context"
	"errors"
	"log"
	"net"
	"sort"
	"strings"
	"time"
)

// **A private name that the system resolves to an address it no longer has.**
//
// The operator, 3 October 2026: a machine deleted and made again under the
// same name in the same project keeps its name (`ollama-53746c3e.internal`)
// and gets a new overlay address. Their Mac was connected to the new machine
// peer to peer, the overlay's own map named the new address, and the browser
// was refused, because something on the device still answered the old one;
// the address typed directly loaded the page. A buyer should never have to
// know that, let alone flush a cache by hand.
//
// So the daemon checks, at start and every minute while the tunnel runs: for
// every peer in the overlay's map, the system resolver's answer (what a
// browser would get) against the address the map holds. A name that resolves
// elsewhere, or not at all, is stale; the system's cache is flushed once and
// the names asked again, and the log says which it was:
//
//	dns: x.internal resolved to [10.212.172.225], the overlay says 10.210.90.192
//	dns: flushed the system cache; x.internal now resolves to 10.210.90.192
//
// or, when a flush does not change the answer, that the stale answer is not
// the system cache's, which is the next place to look. A lookup that times
// out or fails for any reason but "no such host" decides nothing: "could not
// ask" is not "stale".
const (
	dnsCheckEvery  = time.Minute
	dnsLookupLimit = 5 * time.Second
)

// What the check needs from the world, so the tests can be the world.
type dnsWorld struct {
	peers  func() map[string]string // FQDN -> overlay address, from the overlay's own map
	lookup func(ctx context.Context, host string) ([]string, error)
	flush  func() error
}

func systemDNSWorld(t *tunnel) dnsWorld {
	return dnsWorld{
		peers:  t.peers,
		lookup: net.DefaultResolver.LookupHost,
		flush:  flushSystemDNS,
	}
}

// The names the system resolves to anything but the overlay's address, each
// with what it said. A name that does not resolve at all counts: a negative
// answer cached before the machine existed is the same fault.
func staleNames(ctx context.Context, w dnsWorld, peers map[string]string) map[string][]string {
	stale := map[string][]string{}
	for name, want := range peers {
		if name == "" || want == "" {
			continue
		}
		lctx, cancel := context.WithTimeout(ctx, dnsLookupLimit)
		got, err := w.lookup(lctx, name)
		cancel()
		if err != nil {
			var dnsErr *net.DNSError
			if errors.As(err, &dnsErr) && dnsErr.IsNotFound {
				stale[name] = nil
			}
			continue
		}
		if !contains(got, want) {
			sort.Strings(got)
			stale[name] = got
		}
	}
	return stale
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

// One pass: find stale names, flush once, ask again. Returns what was still
// stale after the flush, for the tests and the log.
func checkNames(ctx context.Context, w dnsWorld) map[string][]string {
	peers := w.peers()
	if len(peers) == 0 {
		return nil
	}
	stale := staleNames(ctx, w, peers)
	if len(stale) == 0 {
		return nil
	}
	for _, name := range sortedKeys(stale) {
		log.Printf("dns: %s resolved to %v, the overlay says %s", name, stale[name], peers[name])
	}
	if err := w.flush(); err != nil {
		log.Printf("dns: flushing the system cache failed: %v", err)
		return stale
	}
	after := staleNames(ctx, w, peers)
	for _, name := range sortedKeys(stale) {
		if got, still := after[name]; still {
			log.Printf("dns: flushed the system cache; %s still resolves to %v, so the stale answer is not the system cache's", name, got)
		} else {
			log.Printf("dns: flushed the system cache; %s now resolves to %s", name, peers[name])
		}
	}
	return after
}

func sortedKeys(m map[string][]string) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

// Runs for the life of the daemon. Does nothing while the tunnel is not up:
// with no overlay there is no map to hold a name against.
func watchNames(ctx context.Context, w dnsWorld) {
	ticker := time.NewTicker(dnsCheckEvery)
	defer ticker.Stop()
	for {
		checkNames(ctx, w)
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

// The overlay's map, FQDN to address, while the tunnel runs; empty otherwise.
func (t *tunnel) peers() map[string]string {
	t.mu.Lock()
	c, state := t.client, t.state
	t.mu.Unlock()
	if c == nil || state != stateRunning {
		return nil
	}
	return c.Peers()
}

func (c *embeddedClient) Peers() map[string]string {
	status, err := c.Status()
	if err != nil {
		return nil
	}
	out := map[string]string{}
	for _, p := range status.Peers {
		ip, _, _ := strings.Cut(p.IP, "/")
		if p.FQDN != "" && ip != "" {
			out[strings.TrimSuffix(p.FQDN, ".")] = ip
		}
	}
	return out
}
