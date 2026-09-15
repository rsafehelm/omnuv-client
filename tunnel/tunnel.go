// The private network, embedded.
//
// This is the whole of NetBird's client compiled into one library the desktop
// client loads, replacing the `netbird` CLI it used to drive with QProcess and
// the daemon that CLI needed. One application: no second process to install,
// to keep running, or to explain to a buyer.
//
// **Why a real interface rather than userspace networking.** `client/embed`
// defaults to netstack, where nothing touches the operating system and no
// administrator rights are needed — but traffic then reaches the network only
// through the library's own Dial/ListenUDP, and the video stream is opened by
// moonlight-common-c's own UDP sockets. Those would travel outside the tunnel.
// So `NoUserspace: true`, which creates a genuine WireGuard adapter that
// carries everything the machine sends. The cost is wintun.dll beside the
// binary and the rights to make an adapter, which the installer has and an
// application should not be asking for at run time.
//
// **What this deliberately does not do: allocate anything the caller frees.**
// The library is built with mingw and links the legacy msvcrt, while the Qt
// client uses the UCRT. Memory allocated by one and released by the other is a
// real crash, so every string here is copied into a buffer the caller owns and
// sized. Adding a function that returns a `char*` would reintroduce exactly
// that bug.
//
// **And starting never blocks.** `Start` can take tens of seconds against an
// unreachable management server, and the caller is a UI thread. So the start
// is a goroutine and the caller polls `onv_tunnel_state`, which is the same
// shape the Qt side already had around an asynchronous QProcess.
package main

import "C"

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
	"unsafe"

	netbird "github.com/netbirdio/netbird/client/embed"
)

// The states the caller polls for. They are a superset of "is it up": a
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

var (
	mu        sync.Mutex
	client    *netbird.Client
	state     = stateStopped
	lastError string
)

func setFailed(err error) {
	mu.Lock()
	defer mu.Unlock()
	state = stateFailed
	if err != nil {
		lastError = err.Error()
	}
}

// The identity this device already holds, or "".
//
// **Why this is read here rather than passed in.** A device that has enrolled
// before must come back up without creating anything — `tunnel.h` has always
// drawn that line, and getting it wrong means a fresh peer in the buyer's
// network on every launch, which is the "one orphan peer per machine that ever
// booted" entry in the Inconsistencies list. But `validateCredentials` demands
// one of SetupKey, JWT or PrivateKey *in the options* on every call, and a
// setup key spends itself, so it cannot be the thing that is replayed. The
// stored private key is.
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

//export onv_tunnel_start
//
// Accepts the join and returns at once: 0 accepted, 1 already running or
// starting, 2 the library refused the options, 3 no key was given and this
// device holds no identity to resume — it has never enrolled. The outcome of
// an accepted start arrives through onv_tunnel_state.
//
// `key` empty means *resume*: bring an already-enrolled device back up, which
// must create nothing. A key means *enrol*, which is the only path that makes
// a peer.
func onv_tunnel_start(mgmt, key, name, configDir *C.char) C.int {
	mu.Lock()
	if state == stateStarting || state == stateRunning {
		mu.Unlock()
		return 1
	}
	state = stateStarting
	lastError = ""
	mu.Unlock()

	dir := C.GoString(configDir)
	// **Persisted, always.** With an empty ConfigPath the library keeps its
	// config in memory and nothing survives the process, so every launch would
	// enrol again and leave the last peer behind in the buyer's network.
	configPath := filepath.Join(dir, "config.json")
	statePath := filepath.Join(dir, "state.json")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		setFailed(err)
		return 2
	}

	opts := netbird.Options{
		DeviceName:    C.GoString(name),
		ManagementURL: C.GoString(mgmt),
		ConfigPath:    configPath,
		StatePath:     statePath,
		// A real WireGuard adapter, so the machine's own sockets — the video
		// stream among them — travel through the tunnel. See the note above.
		NoUserspace: true,
	}
	if sk := C.GoString(key); sk != "" {
		opts.SetupKey = sk
	} else if pk := storedIdentity(configPath); pk != "" {
		opts.PrivateKey = pk
	} else {
		mu.Lock()
		state = stateStopped
		lastError = "this device has not joined a network yet"
		mu.Unlock()
		return 3
	}

	c, err := netbird.New(opts)
	if err != nil {
		setFailed(err)
		return 2
	}

	go func() {
		// Generous, because a first enrolment contacts the management server,
		// registers the peer and brings an adapter up. The caller is polling a
		// state rather than waiting on this.
		ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
		defer cancel()
		if err := c.Start(ctx); err != nil {
			setFailed(err)
			return
		}
		mu.Lock()
		client = c
		state = stateRunning
		mu.Unlock()
	}()
	return 0
}

//export onv_tunnel_stop
//
// 0 when there is nothing to stop or it stopped cleanly, 1 when it refused.
func onv_tunnel_stop() C.int {
	mu.Lock()
	c := client
	client = nil
	state = stateStopped
	mu.Unlock()

	if c == nil {
		return 0
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := c.Stop(ctx); err != nil {
		mu.Lock()
		lastError = err.Error()
		mu.Unlock()
		return 1
	}
	return 0
}

//export onv_tunnel_state
func onv_tunnel_state() C.int {
	mu.Lock()
	defer mu.Unlock()
	return C.int(state)
}

//export onv_tunnel_last_error
//
// Copies the last failure into the caller's buffer and returns the number of
// bytes written, never a pointer — see the msvcrt/UCRT note above. A buffer
// too small is truncated rather than refused: this is a sentence for a person,
// not a protocol.
func onv_tunnel_last_error(buf *C.char, length C.int) C.int {
	mu.Lock()
	msg := lastError
	mu.Unlock()
	if buf == nil || length <= 0 {
		return C.int(len(msg))
	}
	b := (*[1 << 20]byte)(unsafe.Pointer(buf))[: length-1 : length-1]
	n := copy(b, msg)
	b2 := (*[1 << 20]byte)(unsafe.Pointer(buf))
	b2[n] = 0
	return C.int(n)
}

func main() {}
