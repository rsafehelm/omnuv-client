//go:build !windows

package main

import (
	"log"
	"net"
	"os"
	"os/signal"
	"runtime"
	"syscall"
)

// The Unix half, which exists for the development loop rather than for a
// product: `scripts/client-linux` ran the client against a real daemon in a
// container, so the socket path the client speaks to was exercised on every
// cycle instead of only on the Windows rig. That loop went on 21 September
// 2026; the macOS build uses this same path now.
func socketName() string {
	if dir := os.Getenv("ONV_TUNNEL_DIR"); dir != "" {
		return dir + "/onv-tunnel.sock"
	}
	return defaultRunDir() + "/onv-tunnel.sock"
}

// **/run on Linux, /var/run on macOS.** macOS has no /run, so both halves
// defaulted to a path that could not exist there; /var/run is where macOS
// keeps runtime sockets. The client's serverName() says the same.
func defaultRunDir() string {
	if runtime.GOOS == "darwin" {
		return "/var/run"
	}
	return "/run"
}

func configDir() string {
	if dir := os.Getenv("ONV_TUNNEL_DIR"); dir != "" {
		return dir + "/state"
	}
	return "/var/lib/onv/tunnel"
}

func listen() (net.Listener, error) {
	// A socket left behind by a process that did not exit cleanly refuses the
	// bind, and "address already in use" on a path nothing is listening to is
	// a confusing way to fail to start.
	_ = os.Remove(socketName())
	ln, err := net.Listen("unix", socketName())
	if err != nil {
		return nil, err
	}
	// **World-connectable on purpose, and not the access control.** The
	// client runs as the person and the daemon as root, and since H4 every
	// request is authorised by the caller's own credentials from the kernel
	// (SO_PEERCRED, owner.go): anyone may ask `state`, and only the tunnel's
	// owner or root may change it. A group would add a per-distribution
	// setup step and decide nothing the owner check does not; the Windows
	// pipe admits every authenticated user for the same reason.
	if err := os.Chmod(socketName(), 0o666); err != nil {
		log.Printf("onv-tunnel: chmod %s: %v", socketName(), err)
	}
	return ln, nil
}

func run(t *tunnel) error {
	// **A stopped service is a stopped tunnel** (4 October 2026). The process
	// used to end on SIGTERM without stopping NetBird, which then never
	// deleted the mapping it held on the router; the Windows service stops
	// the tunnel on the SCM's Stop (main_windows.go), and this is the same
	// for systemd and launchd. Bounded by stopLocked's own 30 s.
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, syscall.SIGTERM, os.Interrupt)
	go func() {
		got := <-signals
		log.Printf("onv-tunnel: %v: stopping the tunnel", got)
		if err := t.stop(); err != nil {
			log.Printf("onv-tunnel: stop on %v: %v", got, err)
		}
		os.Exit(0)
	}()
	t.begin()
	return serve(t)
}
