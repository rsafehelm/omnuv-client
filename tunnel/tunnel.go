// The private network, as a service.
//
// This is NetBird's client (`client/embed`, pinned 0.78.1) hosted in a small
// daemon that owns one thing: the WireGuard adapter this machine reaches its
// Omnuv network through. The desktop client asks it to join, to resume, or for
// its state, over a local socket, and reads the adapter's address from the
// operating system.
//
// **Why a service rather than a library inside the client**, which is what
// this was until it was measured on 16 September. Creating a real WireGuard
// adapter needs administrator rights. In an elevated session the join takes
// about twenty seconds and works; in the ordinary login session the client
// runs in, the identical code hangs for the full ninety seconds and dies with
// `context deadline exceeded`, having logged nothing at all. So the choice was
// never between one process and two — it was between a UAC prompt at every
// login and a privileged helper, and every WireGuard-family client on Windows
// (WireGuard itself, Tailscale, NetBird) answers it the same way.
//
// **Why a real adapter rather than userspace networking.** `client/embed`
// defaults to netstack, where nothing touches the operating system and no
// rights are needed — but traffic then reaches the network only through the
// library's own Dial/ListenUDP, and the video stream is opened by
// moonlight-common-c's own UDP sockets. Those would travel outside the tunnel.
// `NoUserspace: true` creates an adapter that carries everything the machine
// sends, which is the whole point.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	netbird "github.com/netbirdio/netbird/client/embed"
)

// The states the client polls for. They are a superset of "is it up": a
// failure has to be distinguishable from not having started, because the view
// says different things about each, and neither may be reported as "off the
// network" — that is the watcher defect this client has already met once, when
// a killed `netbird status` and a genuinely disconnected device produced the
// same sentence.
const (
	stateStopped  = 0
	stateStarting = 1
	stateRunning  = 2
	stateFailed   = 3
)

// The one refusal that is a question for a person rather than something to
// retry: this machine holds no identity and was given no key, so there is
// nothing to resume and somebody has to fetch it one.
var errNeverEnrolled = errors.New("this device has not joined a network yet")

type tunnel struct {
	mu        sync.Mutex
	client    *netbird.Client
	state     int
	lastError string
}

func (t *tunnel) setFailed(err error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.state = stateFailed
	if err != nil {
		t.lastError = err.Error()
	}
}

func (t *tunnel) snapshot() (int, string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.state, t.lastError
}

// The identity this machine already holds, or "".
//
// **Why this is read here rather than passed in.** A device that has enrolled
// before must come back up without creating anything — getting it wrong means
// a fresh peer in the buyer's network on every start, which is the "one orphan
// peer per machine that ever booted" entry in the Inconsistencies list. But
// `validateCredentials` demands one of SetupKey, JWT or PrivateKey *in the
// options* on every call, and a setup key spends itself, so it cannot be the
// thing that is replayed. The stored private key is.
//
// `profilemanager` is an `internal` package and cannot be imported from
// outside NetBird, so the config is read as what it is on disk: JSON with an
// untagged `PrivateKey` field. If they rename it this returns "" and the
// device asks to be enrolled again rather than silently making a second peer —
// wrong, but wrong in the direction that is visible.
func storedIdentity(configPath string) string {
	raw, err := os.ReadFile(configPath)
	if err != nil {
		return ""
	}
	var cfg struct {
		PrivateKey string
	}
	if err := json.Unmarshal(raw, &cfg); err != nil {
		return ""
	}
	return cfg.PrivateKey
}

// Accepts the join and returns at once; the outcome arrives through the state.
// An empty key means *resume*, which must create nothing.
func (t *tunnel) start(mgmt, key string) error {
	t.mu.Lock()
	if t.state == stateStarting || t.state == stateRunning {
		t.mu.Unlock()
		return nil
	}
	t.state = stateStarting
	t.lastError = ""
	t.mu.Unlock()

	dir := configDir()
	// **Persisted, always.** With an empty ConfigPath the library keeps its
	// config in memory and nothing survives the process, so every start would
	// enrol again and leave the last peer behind in the buyer's network.
	configPath := filepath.Join(dir, "config.json")
	statePath := filepath.Join(dir, "state.json")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.setFailed(err)
		return err
	}

	host, _ := os.Hostname()
	opts := netbird.Options{
		DeviceName:    host,
		ManagementURL: mgmt,
		ConfigPath:    configPath,
		StatePath:     statePath,
		NoUserspace:   true,
	}

	// Exactly one of the two, and which one decides whether a peer is made.
	if key != "" {
		opts.SetupKey = key
	} else if stored := storedIdentity(configPath); stored != "" {
		opts.PrivateKey = stored
	} else {
		t.mu.Lock()
		t.state = stateStopped
		t.lastError = errNeverEnrolled.Error()
		t.mu.Unlock()
		return errNeverEnrolled
	}

	c, err := netbird.New(opts)
	if err != nil {
		t.setFailed(err)
		return err
	}

	go func() {
		// Generous, because a first enrolment contacts the management server,
		// registers the peer and brings an adapter up. The caller polls a
		// state rather than waiting on this.
		ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
		defer cancel()
		if err := c.Start(ctx); err != nil {
			t.setFailed(classify(err))
			return
		}
		t.mu.Lock()
		t.client = c
		t.state = stateRunning
		t.mu.Unlock()
	}()
	return nil
}

// **The one failure the caller must be able to act on differently.**
//
// An identity this machine holds that the server will not accept is not a
// retry: the peer was revoked, or the account was rebuilt, and no amount of
// waiting fixes it. What fixes it is a fresh key — which the caller can ask
// Core for, if somebody is signed in. Every other failure is a retry, and
// asking for a key on one of those would mint a peer per outage, which is the
// orphan this whole design exists to avoid.
//
// **It is a text match, and that is a deliberate choice rather than an
// oversight.** The error arrives as a wrapped gRPC status from inside
// `client/embed`, and matching the code means depending on how deeply NetBird
// wraps it — which is a private detail that changes between versions. The
// sentence is the vendor's too, so this can go stale; when it does, the
// failure is that the caller is *not* told to fetch a key, which leaves
// today's behaviour rather than creating anything.
func classify(err error) error {
	if err == nil {
		return nil
	}
	text := strings.ToLower(err.Error())
	for _, phrase := range []string{
		"invalid setup-key",
		"no sso information",
		"unauthenticated",
		"permissiondenied",
		"permission denied",
	} {
		if strings.Contains(text, phrase) {
			return fmt.Errorf("unauthorized: %w", err)
		}
	}
	return err
}

func (t *tunnel) stop() error {
	t.mu.Lock()
	c := t.client
	t.client = nil
	t.state = stateStopped
	t.mu.Unlock()

	if c == nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := c.Stop(ctx); err != nil {
		t.mu.Lock()
		t.lastError = err.Error()
		t.mu.Unlock()
		return err
	}
	return nil
}
