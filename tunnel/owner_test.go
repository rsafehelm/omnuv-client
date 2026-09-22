package main

import (
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

var (
	alice = caller{id: "S-1-5-21-alice"}
	bob   = caller{id: "S-1-5-21-bob"}
	admin = caller{id: "S-1-5-18", admin: true}
)

// **H4.** The first user to change the tunnel owns it; another may only ask
// whether it is up. Fixtures, not a second Windows account: the identities
// are synthetic, and peerOf is checked against the kernel separately below.
func TestOnlyTheOwnerOrAnAdministratorMayChangeTheTunnel(t *testing.T) {
	tn := testTunnel(t)
	request := enrolRequest{scope(), "https://netbird.lab.omnuv.com", "fixture-key", viewOf(t, tn).Revision}
	if got := answerFor(tn, alice, "enrol-v1 "+payload(request)); got != "ok" {
		t.Fatal(got)
	}
	finish(t, tn)
	if owner, _ := os.ReadFile(filepath.Join(tn.dir, "owner")); strings.TrimSpace(string(owner)) != alice.id {
		t.Fatalf("the first user to join does not own the tunnel: %q", owner)
	}
	for _, line := range []string{"stop", "resume", "membership-v1", "resume-v1 " + payload(scope()),
		"enrol https://netbird.attacker.example key", "stop-v1 " + payload(stopRequest{scope(), viewOf(t, tn).Revision})} {
		if got := answerFor(tn, bob, line); !strings.HasPrefix(got, "err not-owner") {
			t.Fatalf("another user was answered %q to %q", got, strings.Fields(line)[0])
		}
	}
	if state, _ := tn.snapshot(); state != stateRunning {
		t.Fatalf("another user's requests changed the tunnel: %d", state)
	}
	if got := answerFor(tn, bob, "state"); !strings.HasPrefix(got, "state 2 ") {
		t.Fatalf("another user may still ask whether it is up: %q", got)
	}
	// An unknown caller is nobody's owner, so it is refused too.
	if got := answerFor(tn, caller{}, "stop"); !strings.HasPrefix(got, "err not-owner") {
		t.Fatalf("an unidentified caller stopped an owned tunnel: %q", got)
	}
	// The owner and an administrator may; neither moves the ownership.
	if got := answerFor(tn, alice, "membership-v1"); strings.HasPrefix(got, "err") {
		t.Fatal(got)
	}
	if got := answerFor(tn, admin, "stop"); got != "ok" {
		t.Fatal(got)
	}
	if owner, _ := os.ReadFile(filepath.Join(tn.dir, "owner")); strings.TrimSpace(string(owner)) != alice.id {
		t.Fatalf("an administrator's request moved the ownership: %q", owner)
	}
}

// An administrator, the installer, joining first claims nothing, so the first
// person to use it afterwards becomes the owner.
func TestAnAdministratorClaimsNothing(t *testing.T) {
	tn := testTunnel(t)
	if got := answerFor(tn, admin, "enrol https://netbird.lab.omnuv.com fixture-key"); got != "ok" {
		t.Fatal(got)
	}
	finish(t, tn)
	if _, err := os.Stat(filepath.Join(tn.dir, "owner")); !os.IsNotExist(err) {
		t.Fatal("an administrator became the tunnel's owner")
	}
}

// peerOf reads the kernel's record of the other end, on a real socket: here,
// this test's own uid.
func TestThePeerIsWhoTheKernelSaysItIs(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("peer credentials are read on Linux and Windows only")
	}
	path := filepath.Join(t.TempDir(), "s")
	ln, err := net.Listen("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	got := make(chan caller, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			got <- caller{id: "accept failed"}
			return
		}
		defer conn.Close()
		got <- peerOf(conn)
	}()
	conn, err := net.Dial("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	who := <-got
	if who.id != strconv.Itoa(os.Getuid()) || who.admin != (os.Getuid() == 0) {
		t.Fatalf("peer %+v, expected uid %d", who, os.Getuid())
	}
}
