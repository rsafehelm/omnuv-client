package main

// Whether this device may ask its router to open a port: the buyer's choice,
// off until they make it.
//
// **Why it is off by default, and was not until 4 October 2026.** NetBird
// 0.78.1 starts its port mapper on every connection (`client/internal/
// engine.go:661`) unless the environment says `NB_DISABLE_NAT_MAPPER`
// (`portforward/env.go:11`, read by `manager.go:72` each time an engine
// starts). This daemon set nothing, so every buyer's device asked its
// router for a two-hour UDP mapping without the buyer having been asked
// (omnuv: docs/research/2026-10-04-overlay-path-and-upnp.md). A mapping on a
// buyer's router is the buyer's to allow.
//
// **What turning it on buys.** A direct path needs only one of the two ends
// to be reachable. A machine behind a symmetric NAT — the operator's own
// site — cannot be reached, and until something opens one side the stream
// goes through a relay. When this device's router opens the WireGuard port,
// NetBird offers the mapped address to the other end as a candidate at
// priority +1000 (`peer/worker_ice.go:499`), and the other end can reach it.
//
// **What it opens, and only that.** One UDP mapping, for the WireGuard port
// the tunnel listens on (`manager.go:182`: `AddPortMapping(ctx, "udp",
// wgPort, …)`), where WireGuard answers nothing that is not authenticated.
// NetBird may also open an IPv6 pinhole for that same port over PCP. It is
// renewed while the tunnel runs and deleted when it stops: by NetBird, and
// for what NetBird leaves behind, by portmap_release.go.
//
// **Kept per identity.** The choice is stored beside the identity it was
// made for, keyed by that identity's fingerprint: a new enrolment is a new
// device on the network, and inherits nothing — the rule that assets are
// referred to by id, never by where they happen to be stored.

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"os"
	"path/filepath"
	"time"
)

const natMapperEnv = "NB_DISABLE_NAT_MAPPER"

type portmapConsent struct {
	Identity string    `json:"identity"` // hashText of the private key it was given for
	Allowed  bool      `json:"allowed"`
	At       time.Time `json:"at"`
}

func consentPath(dir string) string { return filepath.Join(dir, "portmap-consent.json") }

func (t *tunnel) leaseRecordPath() string { return filepath.Join(t.base(), "portmap-lease.json") }

// Whether the owner allowed a mapping for this identity. Anything else —
// no file, an unreadable one, another identity's — is no.
func readConsent(dir, identity string) bool {
	if identity == "" {
		return false
	}
	raw, err := os.ReadFile(consentPath(dir))
	if err != nil {
		return false
	}
	var c portmapConsent
	if json.Unmarshal(raw, &c) != nil {
		return false
	}
	return c.Allowed && c.Identity == hashText(identity)
}

// The switch NetBird reads, set before every start. Explicit both ways, so
// an environment inherited from whoever launched the service decides
// nothing.
func setNATMapper(allowed bool) {
	value := "true"
	if allowed {
		value = "false"
	}
	_ = os.Setenv(natMapperEnv, value)
}

// The observer, made once, on first use.
func (t *tunnel) portmapObs() *portmapObserver {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.portmap == nil {
		t.portmap = newPortmapObserver(t.leaseRecordPath())
	}
	return t.portmap
}

// Caller holds operations. Before a start: the switch is decided for this
// identity, and only then may NetBird read it. A start with a setup key is a
// new identity, which has not been allowed anything.
func (t *tunnel) preparePortmapLocked(dir, identity string, keyed bool) {
	allowed := !keyed && readConsent(dir, identity)
	setNATMapper(allowed)
	t.portmapObs().begin()
	t.mu.Lock()
	t.mapperOn = allowed
	t.mu.Unlock()
	log.Printf("onv-tunnel: router port mapping %s for this start", onOff(allowed))
}

// Caller holds operations, with no client running. Whatever a stopped tunnel
// left on the router is removed now, and what could not be is said.
func (t *tunnel) settlePortmapLocked() {
	obs := t.portmapObs()
	obs.stopped()
	t.mu.Lock()
	t.mapperOn = false
	t.mu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()
	said := obs.settle(ctx, time.Now().UTC())
	if said == "" {
		return
	}
	log.Printf("onv-tunnel: router port mapping: %s", said)
	t.mu.Lock()
	t.portmapLeftover = said
	t.mu.Unlock()
}

// The owner's choice, from `portmap-v1 on|off`. A running tunnel is
// restarted when the choice changes what it runs with: NetBird reads the
// switch only when an engine starts, and an owner who says no must not
// leave a mapping open until the next reconnection.
func (t *tunnel) setPortMapping(allowed bool) error {
	t.operations.Lock()
	defer t.operations.Unlock()
	if err := t.recoverAsideLocked(); err != nil {
		return err
	}
	t.mu.Lock()
	dir := t.directory()
	state, running := t.state, t.mapperOn
	t.mu.Unlock()
	identity, err := readIdentity(filepath.Join(dir, "config.json"))
	if err != nil {
		return err
	}
	if identity == "" {
		return errors.New("join this device to a network first: the choice is kept for this device's identity")
	}
	raw, err := json.Marshal(portmapConsent{Identity: hashText(identity), Allowed: allowed, At: time.Now().UTC()})
	if err != nil {
		return err
	}
	if err := writeFileAtomic(consentPath(dir), raw); err != nil {
		return err
	}
	log.Printf("onv-tunnel: router port mapping turned %s by the device's owner", onOff(allowed))
	if (state != stateRunning && state != stateStarting) || running == allowed {
		return nil
	}
	if err := t.stopLocked(); err != nil {
		return err
	}
	return t.resumeLocked("", "")
}

type portmapView struct {
	// The owner's choice for the identity in use.
	Allowed bool `json:"allowed"`
	// What the running tunnel was started with: "on" or "off"; "" when no
	// tunnel runs.
	Mapper string `json:"mapper"`
	portmapState
	// The last attempt to remove what a stopped tunnel left on the router.
	Leftover string `json:"leftover,omitempty"`
}

func (t *tunnel) portmapView() (string, error) {
	obs := t.portmapObs()
	t.mu.Lock()
	dir := t.directory()
	state, on, leftover := t.state, t.mapperOn, t.portmapLeftover
	t.mu.Unlock()
	identity, err := readIdentity(filepath.Join(dir, "config.json"))
	if err != nil {
		return "", err
	}
	view := portmapView{Allowed: readConsent(dir, identity), portmapState: obs.snapshot(), Leftover: leftover}
	if state == stateRunning || state == stateStarting {
		view.Mapper = onOff(on)
	}
	raw, err := json.Marshal(view)
	if err != nil {
		return "", errors.New("port mapping observation unavailable")
	}
	return string(raw), nil
}

func onOff(on bool) string {
	if on {
		return "on"
	}
	return "off"
}
