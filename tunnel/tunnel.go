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
	"errors"
	"fmt"
	"log"
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

type tunnelClient interface {
	Start(context.Context) error
	Stop(context.Context) error
	Address() (string, string)
	Identity() (string, error)
}

type embeddedClient struct {
	*netbird.Client
	stopError error
}

func (c *embeddedClient) Stop(ctx context.Context) error {
	// NetBird clears its internal pointer even when Stop times out. A second
	// "not started" would not prove that the earlier cleanup finished.
	if c.stopError != nil {
		return fmt.Errorf("previous tunnel stop is unresolved; restart the daemon before replacing identity: %w", c.stopError)
	}
	err := c.Client.Stop(ctx)
	if errors.Is(err, netbird.ErrClientNotStarted) {
		return nil
	}
	if err != nil {
		c.stopError = err
	}
	return err
}

func (c *embeddedClient) Address() (string, string) {
	status, err := c.Status()
	if err != nil {
		return "", ""
	}
	ip, _, _ := strings.Cut(status.LocalPeerState.IP, "/")
	return ip, status.LocalPeerState.FQDN
}

func (c *embeddedClient) Identity() (string, error) {
	config, err := c.GetConfig()
	if err != nil {
		return "", err
	}
	return config.PrivateKey, nil
}

type tunnel struct {
	// operations serializes accepted start/stop changes; mu protects observation
	// and completion. A stopped generation can never publish a later success.
	operations sync.Mutex
	mu         sync.Mutex
	client     tunnelClient
	state      int
	lastError  string
	generation uint64
	cancel     context.CancelFunc
	done       chan struct{}
	dir        string
	factory    func(netbird.Options) (tunnelClient, error)
	// The deployment whose identity is in use: a Core origin, or "" for the
	// base directory (an identity no membership ever named). Read lazily from
	// <base>/active and migrated on first use; see deployments.go.
	active       string
	activeLoaded bool
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

// The library's status recorder is authoritative; an OS adapter may lag it.
func (t *tunnel) address() (string, string) {
	t.mu.Lock()
	c, state := t.client, t.state
	t.mu.Unlock()
	if c == nil || state != stateRunning {
		return "", ""
	}
	return c.Address()
}

// Legacy callers and service startup can resume an existing identity, but
// cannot label it with a project. Versioned clients require its stored binding.
func (t *tunnel) start(mgmt, key string) error {
	t.operations.Lock()
	defer t.operations.Unlock()
	if key != "" && !origin(mgmt) {
		return errors.New("enrollment requires a management HTTPS origin")
	}
	if err := t.recoverAsideLocked(); err != nil {
		return err
	}
	var record *membershipRecord
	if key == "" {
		t.mu.Lock()
		view, saved, err := t.membershipLocked()
		t.mu.Unlock()
		if err != nil {
			return err
		}
		if saved != nil && !view.Verified {
			return errors.New("membership-unverified: incomplete enrollment needs confirmation")
		}
		if view.Verified {
			record = saved
			mgmt = saved.ManagementURL
		}
	}
	return t.startLocked(mgmt, key, record)
}

// Caller holds operations. Removal failures stop the transition; an old key
// must never silently win over a newly supplied setup key.
func (t *tunnel) startLocked(mgmt, key string, record *membershipRecord) error {
	t.mu.Lock()
	active := t.state == stateStarting || t.state == stateRunning
	t.mu.Unlock()
	if key == "" && active {
		return nil
	}
	if err := t.stopLocked(); err != nil {
		return err
	}
	dir := t.directory()
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.setFailed(err)
		return err
	}
	configPath, statePath := filepath.Join(dir, "config.json"), filepath.Join(dir, "state.json")
	// A keyed attempt that fails before its goroutine runs gives the identity
	// it set aside back (aside.go).
	keyed := key != ""
	fail := func(err error) error {
		if keyed {
			if restoreErr := restoreAside(dir); restoreErr != nil {
				err = errors.Join(err, restoreErr)
			}
		}
		t.setFailed(err)
		return err
	}
	if keyed {
		// Set the identity aside rather than deleting it, and invalidate the
		// binding with it: the files move together, so a reader sees either the
		// old identity or none, never a new scope on an old key.
		if err := setAside(dir); err != nil {
			t.setFailed(err)
			return err
		}
		if record != nil {
			if err := t.writeMembership(*record); err != nil {
				return fail(err)
			}
		}
	}
	host, _ := os.Hostname()
	opts := netbird.Options{DeviceName: host, ManagementURL: mgmt, ConfigPath: configPath, StatePath: statePath, NoUserspace: true}
	if key != "" {
		opts.SetupKey = key
	} else {
		identity, err := readIdentity(configPath)
		if err != nil {
			t.setFailed(err)
			return err
		}
		if identity == "" {
			t.mu.Lock()
			t.state = stateStopped
			t.lastError = errNeverEnrolled.Error()
			t.mu.Unlock()
			return errNeverEnrolled
		}
		opts.PrivateKey = identity
		if record != nil && record.IdentityHash != hashText(identity) {
			err := errors.New("membership-changed: stored private identity no longer matches its scope")
			t.setFailed(err)
			return err
		}
	}
	factory := t.factory
	if factory == nil {
		factory = func(options netbird.Options) (tunnelClient, error) {
			client, err := netbird.New(options)
			if err != nil {
				return nil, err
			}
			return &embeddedClient{Client: client}, nil
		}
	}
	log.Printf("onv-tunnel: starting key=%t verified-scope=%t", key != "", record != nil)
	client, err := factory(opts)
	if err != nil {
		return fail(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	t.mu.Lock()
	t.generation++
	generation := t.generation
	done := make(chan struct{})
	t.client, t.cancel, t.done = client, cancel, done
	t.state, t.lastError = stateStarting, ""
	t.mu.Unlock()
	go func() {
		defer close(done)
		defer cancel()
		err := client.Start(ctx)
		t.mu.Lock()
		if generation != t.generation {
			t.mu.Unlock()
			return
		}
		if err == nil {
			err = ctx.Err()
		}
		if err == nil && record != nil {
			identity, readErr := client.Identity()
			if readErr != nil {
				err = readErr
			} else if identity == "" {
				err = errors.New("successful tunnel start did not report its identity")
			} else {
				stored, storedErr := readIdentity(configPath)
				if storedErr != nil {
					err = storedErr
				} else if stored != identity {
					err = errors.New("stored identity differs from the running tunnel; refusing a false scope binding")
				} else {
					record.IdentityHash, record.Verified = hashText(identity), true
					err = t.writeMembership(*record)
				}
			}
		}
		if err != nil {
			t.state, t.lastError = stateFailed, classify(err).Error()
			t.mu.Unlock()
			// A scope persistence error after Start succeeded must not leave
			// an unverified adapter running. stopLocked waits for done before
			// touching this client, so this cleanup cannot overlap another Stop.
			cleanup, cancelCleanup := context.WithTimeout(context.Background(), 30*time.Second)
			stopErr := client.Stop(cleanup)
			cancelCleanup()
			t.mu.Lock()
			if generation == t.generation {
				if stopErr != nil {
					t.lastError += "; tunnel cleanup failed: " + stopErr.Error()
				} else if keyed {
					// The key did not work: the identity it was to replace
					// comes back. A newer operation that got here first
					// restores it itself, under operations.
					if restoreErr := restoreAside(dir); restoreErr != nil {
						t.lastError += "; " + restoreErr.Error()
					} else {
						t.lastError += "; the previous identity was kept"
					}
				}
			}
			t.mu.Unlock()
			log.Printf("onv-tunnel: start failed: %v", err)
			return
		}
		if keyed {
			if dropErr := dropAside(dir); dropErr != nil {
				log.Printf("onv-tunnel: the replaced identity could not be removed: %v", dropErr)
			}
		}
		t.state = stateRunning
		t.mu.Unlock()
		log.Printf("onv-tunnel: running")
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

// Caller holds operations. Wait for the canceled start before stopping the
// library, so an old goroutine cannot revive the adapter after a newer start.
func (t *tunnel) stopLocked() error {
	t.mu.Lock()
	t.generation++
	client, cancel, done := t.client, t.cancel, t.done
	if cancel != nil {
		cancel()
	}
	t.mu.Unlock()
	ctx, cancelStop := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancelStop()
	if done != nil {
		select {
		case <-done:
		case <-ctx.Done():
			err := errors.New("previous tunnel start did not stop; refusing overlapping transition")
			t.setFailed(err)
			return err
		}
	}
	if client != nil {
		if err := client.Stop(ctx); err != nil {
			t.setFailed(err)
			return err
		}
	}
	t.mu.Lock()
	t.client, t.cancel, t.done = nil, nil, nil
	t.state, t.lastError = stateStopped, ""
	t.mu.Unlock()
	return nil
}

func (t *tunnel) stop() error {
	t.operations.Lock()
	defer t.operations.Unlock()
	return t.stopLocked()
}
