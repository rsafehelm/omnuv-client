//go:build darwin

package main

import (
	"net"
	"strconv"

	"golang.org/x/sys/unix"
)

// The kernel's word for who is on the other end of the socket, as on Linux:
// LOCAL_PEERCRED is filled in at connect and cannot be forged.
//
// **Until 25 September 2026 macOS read nothing** (peer_other.go): every
// caller was unknown, an unknown caller never becomes the owner, and so on a
// Mac the tunnel was never owned and any local account could change it. H4's
// owner check existed on Linux and Windows only.
func peerOf(conn net.Conn) caller {
	unixConn, ok := conn.(*net.UnixConn)
	if !ok {
		return caller{}
	}
	raw, err := unixConn.SyscallConn()
	if err != nil {
		return caller{}
	}
	var cred *unix.Xucred
	var credErr error
	if err := raw.Control(func(fd uintptr) {
		cred, credErr = unix.GetsockoptXucred(int(fd), unix.SOL_LOCAL, unix.LOCAL_PEERCRED)
	}); err != nil || credErr != nil || cred == nil {
		return caller{}
	}
	return caller{id: strconv.FormatUint(uint64(cred.Uid), 10), admin: cred.Uid == 0}
}
