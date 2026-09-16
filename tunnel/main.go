package main

import (
	"log"
	"os"
	"path/filepath"
)

// `onvtunneld` — the privileged half of the private network.
//
//	onvtunneld            run it: as a Windows service when the SCM started
//	                      us, in the foreground otherwise
//
// It takes no arguments on purpose. Everything it does is asked for over its
// socket by the desktop client, and a daemon with a command line is a daemon
// somebody configures differently on one machine.
func main() {
	// **A service's stderr goes nowhere, and that is where its diagnosis
	// lives.** Run by the Windows SCM there is no console and no redirection,
	// so every line this daemon wrote about a refused key or a failed start
	// was discarded — and on 16 September a join that reported success and did
	// not happen could not be explained from the outside at all.
	//
	// Beside the identity it already owns, so there is one directory to look
	// in. Falls back to stderr when the file cannot be opened, because a
	// daemon that will not start for want of a log is worse than a quiet one.
	log.SetFlags(log.LstdFlags | log.LUTC)
	if err := os.MkdirAll(configDir(), 0o700); err == nil {
		if f, err := os.OpenFile(
			filepath.Join(configDir(), "onvtunneld.log"),
			os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o640,
		); err == nil {
			log.SetOutput(f)
		} else {
			log.SetOutput(os.Stderr)
		}
	} else {
		log.SetOutput(os.Stderr)
	}
	t := &tunnel{state: stateStopped}
	if err := run(t); err != nil {
		log.Printf("onv-tunnel: %v", err)
		os.Exit(1)
	}
}
