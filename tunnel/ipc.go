package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"strings"
)

// One command per line. Versioned scope commands use base64 JSON payloads.
//
// One request per connection, answered and closed. Nothing here is a stream,
// nothing needs framing, and a text protocol can be spoken by hand with a pipe
// client when something is wrong — which is the state this will most often be
// looked at in.
//
//	resume                 → ok | err <sentence>
//	enrol <url> <key>      → ok | err <sentence>
//	stop                   → ok | err <sentence>
//	state                  → state <0-3> <address|-> <name|-> <sentence, possibly empty>
//	membership-v1          → JSON public scope, identity presence and CAS revision
//	resume-v1 <payload>    → ok | err <sentence> (exact verified membership)
//	enrol-v1 <payload>     → ok | err <sentence> (confirmed revision replacement)
//	stop-v1 <payload>      → ok | err <sentence> (matching membership + revision)
//
// The address and the name are this device's own, taken from the client's
// status recorder rather than from the machine's interface list — see
// `tunnel.address()`. Both are `-` when the client does not have them yet,
// which is a different thing from a tunnel that is down and must stay
// distinguishable: a field that is empty and a field that is absent read the
// same to a parser splitting on spaces.
//
// `resume` and `enrol` return as soon as the attempt is *accepted*; whether it
// worked arrives through `state`, because a join takes tens of seconds and the
// caller is a user interface.
//
// **What this does not do is authenticate.** The socket's own permissions are
// the whole access control: on Windows the pipe grants Administrators and
// authenticated users, on Unix the socket is 0660 and root-owned. On a
// single-person machine that is the right boundary — anyone who can reach it
// can already read the buyer's files. On a shared machine it means any local
// user can take the tunnel down or join it to a network whose key they hold,
// and that is worth knowing before this ships anywhere multi-user.
func serve(t *tunnel) error {
	ln, err := listen()
	if err != nil {
		return err
	}
	defer ln.Close()
	log.Printf("onv-tunnel: listening on %s", socketName())

	for {
		conn, err := ln.Accept()
		if err != nil {
			// A closed listener is how this shuts down; anything else is worth
			// saying out loud before the loop ends.
			log.Printf("onv-tunnel: accept: %v", err)
			return err
		}
		go handle(t, conn)
	}
}

func handle(t *tunnel, conn net.Conn) {
	defer conn.Close()
	line, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil && line == "" {
		return
	}
	fmt.Fprintln(conn, answer(t, strings.TrimSpace(line)))
}

// Split out from the connection so it can be tested without a socket, which is
// the difference between a protocol with a check and one with a comment saying
// what it would do.
func answer(t *tunnel, line string) string {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return "err empty request"
	}

	switch fields[0] {
	case "membership-v1":
		if len(fields) != 1 {
			return "err membership-v1 takes no arguments"
		}
		view, err := t.membershipSnapshot()
		if err != nil {
			return "err " + err.Error()
		}
		raw, err := json.Marshal(view)
		if err != nil {
			return "err membership observation unavailable"
		}
		return string(raw)

	case "resume-v1", "enrol-v1", "stop-v1":
		if len(fields) != 2 {
			return "err versioned request takes one base64 JSON payload"
		}
		var err error
		switch fields[0] {
		case "resume-v1":
			var request membership
			if err = decodeRequest(fields[1], &request); err == nil {
				err = t.resumeMembership(request)
			}
		case "enrol-v1":
			var request enrolRequest
			if err = decodeRequest(fields[1], &request); err == nil {
				err = t.enrolMembership(request)
			}
		case "stop-v1":
			var request stopRequest
			if err = decodeRequest(fields[1], &request); err == nil {
				err = t.stopMembership(request)
			}
		}
		if err != nil {
			return "err " + err.Error()
		}
		return "ok"

	case "state":
		state, why := t.snapshot()
		ip, fqdn := t.address()
		return fmt.Sprintf("state %d %s %s %s", state, dash(ip), dash(fqdn), why)

	case "resume":
		if err := t.start("", ""); err != nil {
			// **One refusal is a question, and it is marked.** Everything else
			// is worth retrying; this one is not, because the machine has no
			// identity and nothing the daemon can do will make one. Whoever
			// has a session fetches a key. The caller must be able to tell
			// them apart without matching on a sentence, which is why this is
			// a token rather than prose.
			if errors.Is(err, errNeverEnrolled) {
				return "err need-key " + err.Error()
			}
			return "err " + err.Error()
		}
		return "ok"

	case "enrol":
		// The url and the key, both required. **A key with no address is
		// refused rather than guessed at**: the library would fall back to the
		// vendor's own cloud, enrol there, and report success — a device on
		// somebody else's network, which is worse than a failure because it
		// looks like one working.
		if len(fields) != 3 {
			return "err enrol takes a management url and a setup key"
		}
		if err := t.start(fields[1], fields[2]); err != nil {
			return "err " + err.Error()
		}
		return "ok"

	case "stop":
		if err := t.stop(); err != nil {
			return "err " + err.Error()
		}
		return "ok"
	}
	return "err unknown request: " + fields[0]
}

// An empty field would vanish when the line is split on spaces, taking the
// fields after it one place to the left. `-` is never a valid address or name.
func dash(s string) string {
	if s == "" {
		return "-"
	}
	return s
}
