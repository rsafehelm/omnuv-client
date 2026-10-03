package main

import (
	"fmt"
	"os/exec"
)

// macOS keeps resolved names in Directory Services' cache and in
// mDNSResponder; Apple's own instruction for clearing both is these two
// commands, and neither has a library call a pure-Go binary can reach.
func flushSystemDNS() error {
	if out, err := exec.Command("/usr/bin/dscacheutil", "-flushcache").CombinedOutput(); err != nil {
		return fmt.Errorf("dscacheutil -flushcache: %v: %s", err, out)
	}
	if out, err := exec.Command("/usr/bin/killall", "-HUP", "mDNSResponder").CombinedOutput(); err != nil {
		return fmt.Errorf("killall -HUP mDNSResponder: %v: %s", err, out)
	}
	return nil
}
