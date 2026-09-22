//go:build !linux && !windows

package main

import "net"

// No peer credentials read here yet: every caller is unknown, so it may act
// only while nobody owns the tunnel. This platform's daemon is a development
// path (main_unix.go).
func peerOf(net.Conn) caller { return caller{} }
