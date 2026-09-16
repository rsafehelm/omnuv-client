package main

import (
	"bufio"
	"errors"
	"fmt"
	"log"
	"net"
	"strings"
)

// The protocol, and it is deliberately four words on a line.
//
// One request per connection, answered and closed. Nothing here is a stream,
// nothing needs framing, and a text protocol can be spoken by hand with a pipe
// client when something is wrong — which is the state this will most often be
// looked at in.
//
//	resume                 → ok | err <sentence>
//	enrol <url> <key>      → ok | err <sentence>
//	stop                   → ok | err <sentence>
//	state                  → state <0-3> <sentence, possibly empty>
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
	case "state":
		state, why := t.snapshot()
		return fmt.Sprintf("state %d %s", state, why)

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
