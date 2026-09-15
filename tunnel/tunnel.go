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

//export onv_tunnel_start
//
// Accepts the join and returns at once: 0 accepted, 1 already running or
// starting, 2 the library refused the options. The outcome arrives through
// onv_tunnel_state.
func onv_tunnel_start(mgmt, key, name *C.char) C.int {
	mu.Lock()
	if state == stateStarting || state == stateRunning {
		mu.Unlock()
		return 1
	}
	state = stateStarting
	lastError = ""
	mu.Unlock()

	c, err := netbird.New(netbird.Options{
		DeviceName:    C.GoString(name),
		SetupKey:      C.GoString(key),
		ManagementURL: C.GoString(mgmt),
		// A real WireGuard adapter, so the machine's own sockets — the video
		// stream among them — travel through the tunnel. See the note above.
		NoUserspace: true,
	})
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
