//go:build !darwin && !windows && !linux

package main

// No system cache this daemon knows how to clear.
func flushSystemDNS() error { return nil }
