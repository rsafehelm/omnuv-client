package main

import (
	"log"
	"os"
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
	log.SetOutput(os.Stderr)
	log.SetFlags(log.LstdFlags | log.LUTC)
	t := &tunnel{state: stateStopped}
	if err := run(t); err != nil {
		log.Printf("onv-tunnel: %v", err)
		os.Exit(1)
	}
}
