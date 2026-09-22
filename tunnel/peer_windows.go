package main

import (
	"net"

	"golang.org/x/sys/windows"
)

// The pipe client's process, its token, and from the token the user's SID
// and whether it is an elevated administrator or SYSTEM. Read from the
// operating system, never from anything the client says.
func peerOf(conn net.Conn) caller {
	file, ok := conn.(interface{ Fd() uintptr })
	if !ok {
		return caller{}
	}
	var pid uint32
	if err := windows.GetNamedPipeClientProcessId(windows.Handle(file.Fd()), &pid); err != nil {
		return caller{}
	}
	process, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
	if err != nil {
		return caller{}
	}
	defer windows.CloseHandle(process)
	var token windows.Token
	if err := windows.OpenProcessToken(process, windows.TOKEN_QUERY, &token); err != nil {
		return caller{}
	}
	defer token.Close()
	user, err := token.GetTokenUser()
	if err != nil {
		return caller{}
	}
	sid := user.User.Sid.String()
	system := user.User.Sid.IsWellKnown(windows.WinLocalSystemSid)
	return caller{id: sid, admin: system || token.IsElevated()}
}
