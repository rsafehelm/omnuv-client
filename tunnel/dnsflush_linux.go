package main

import (
	"fmt"
	"os/exec"
)

// systemd-resolved is the cache on the distributions the client supports;
// `resolvectl flush-caches` is its own way of clearing it. Without resolved
// there is no system cache to clear, and that is not a failure.
func flushSystemDNS() error {
	path, err := exec.LookPath("resolvectl")
	if err != nil {
		return nil
	}
	if out, err := exec.Command(path, "flush-caches").CombinedOutput(); err != nil {
		return fmt.Errorf("resolvectl flush-caches: %v: %s", err, out)
	}
	return nil
}
