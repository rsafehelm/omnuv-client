//go:build linux

package main

import (
	"net"
	"strconv"

	"golang.org/x/sys/unix"
)

// The kernel's word for who is on the other end of the socket, not the
// caller's: SO_PEERCRED is filled in at connect and cannot be forged.
func peerOf(conn net.Conn) caller {
	unixConn, ok := conn.(*net.UnixConn)
	if !ok {
		return caller{}
	}
	raw, err := unixConn.SyscallConn()
	if err != nil {
		return caller{}
	}
	var cred *unix.Ucred
	var credErr error
	if err := raw.Control(func(fd uintptr) {
		cred, credErr = unix.GetsockoptUcred(int(fd), unix.SOL_SOCKET, unix.SO_PEERCRED)
	}); err != nil || credErr != nil || cred == nil {
		return caller{}
	}
	return caller{id: strconv.FormatUint(uint64(cred.Uid), 10), admin: cred.Uid == 0}
}
