//go:build !windows

package main

import "os"

// secureDir makes dir, and leaves it readable by its owner alone: the
// identity in it is this device's WireGuard private key. MkdirAll's mode
// applies only to what it creates, so an existing directory is set too.
func secureDir(dir string) error {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	return os.Chmod(dir, 0o700)
}
