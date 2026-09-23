//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"testing"
)

// The key's directory is its owner's alone, including one that already
// existed with a wider mode: MkdirAll leaves an existing directory as it is.
func TestTheIdentityDirectoryIsItsOwnersAlone(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "state")
	if err := os.Mkdir(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := secureDir(dir); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(dir)
	if err != nil {
		t.Fatal(err)
	}
	if got := info.Mode().Perm(); got != 0o700 {
		t.Fatalf("mode %o, want 700", got)
	}
}
