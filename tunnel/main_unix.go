//go:build !windows

package main

import (
	"log"
	"net"
	"os"
)

// The Unix half, which exists for the development loop rather than for a
// product: `scripts/client-linux` runs the client against a real daemon in a
// container, so the socket path the client speaks to is exercised on every
// cycle instead of only on the Windows rig.
func socketName() string {
	if dir := os.Getenv("ONV_TUNNEL_DIR"); dir != "" {
		return dir + "/onv-tunnel.sock"
	}
	return "/run/onv-tunnel.sock"
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
	// The client runs as the person; the daemon runs as root. 0660 plus a
	// group is the usual answer, but the group differs per distribution and
	// this is a development path, so it is world-writable and says so.
	// ponytail: 0666 on the dev socket; a group of our own if this ever ships
	// on Linux as a product.
	if err := os.Chmod(socketName(), 0o666); err != nil {
		log.Printf("onv-tunnel: chmod %s: %v", socketName(), err)
	}
	return ln, nil
}

func run(t *tunnel) error {
	go func() {
		if err := t.start("", ""); err != nil {
			log.Printf("onv-tunnel: nothing to resume: %v", err)
		}
	}()
	return serve(t)
}
