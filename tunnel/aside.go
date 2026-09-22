package main

import (
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
)

// **A key is tried before the identity it replaces is given up (H3, 22
// September 2026).** A keyed start used to delete the membership, config and
// state first and then try the key. A junk key, a spent one, or one for an
// attacker's network sent from a web page therefore cost the device its place
// on every network, whatever the key turned out to be.
//
// Now the three files are moved into <dir>/previous before the attempt:
//
//	the attempt runs          previous is kept
//	the attempt fails         previous is moved back, and the attempt's files go
//	the attempt succeeds      previous is removed
//	the attempt is cancelled  previous stays; the next start under operations
//	or the daemon dies         restores it unless a verified membership replaced it
//
// A previous directory that already exists holds an older identity from an
// attempt that never resolved, and it is the one worth keeping, so it is not
// overwritten.

var identityFiles = []string{"membership.json", "config.json", "state.json"}

func asideDir(dir string) string { return filepath.Join(dir, "previous") }

// Caller holds operations. Moves the identity out of the way of a keyed
// attempt. On failure nothing has moved: what was moved is moved back.
func setAside(dir string) error {
	aside := asideDir(dir)
	if _, err := os.Stat(aside); err == nil {
		// An unresolved earlier attempt: keep the older identity it holds and
		// clear only the attempt's own files.
		for _, name := range identityFiles {
			if err := removeIfPresent(filepath.Join(dir, name)); err != nil {
				return err
			}
		}
		return nil
	}
	if err := os.Mkdir(aside, 0o700); err != nil {
		return fmt.Errorf("set the current identity aside: %w", err)
	}
	var moved []string
	for _, name := range identityFiles {
		from := filepath.Join(dir, name)
		if _, err := os.Lstat(from); os.IsNotExist(err) {
			continue
		}
		if err := os.Rename(from, filepath.Join(aside, name)); err != nil {
			for _, back := range moved {
				_ = os.Rename(filepath.Join(aside, back), filepath.Join(dir, back))
			}
			_ = os.Remove(aside)
			return fmt.Errorf("set the current identity aside: %w", err)
		}
		moved = append(moved, name)
	}
	return nil
}

// The attempt failed: the identity it replaced comes back. When there was
// none, the attempt's own files stay, so a first enrolment that failed still
// shows its pending record.
func restoreAside(dir string) error {
	aside := asideDir(dir)
	entries, err := os.ReadDir(aside)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if len(entries) == 0 {
		return os.Remove(aside)
	}
	for _, name := range identityFiles {
		if err := os.RemoveAll(filepath.Join(dir, name)); err != nil {
			return err
		}
	}
	for _, name := range identityFiles {
		from := filepath.Join(aside, name)
		if _, err := os.Lstat(from); os.IsNotExist(err) {
			continue
		}
		if err := os.Rename(from, filepath.Join(dir, name)); err != nil {
			return fmt.Errorf("restore the previous identity: %w", err)
		}
	}
	return os.Remove(aside)
}

// The attempt succeeded: what it replaced is gone for good.
func dropAside(dir string) error {
	return os.RemoveAll(asideDir(dir))
}

// Caller holds operations. An attempt that neither succeeded nor failed — it
// was cancelled, or the daemon died — leaves previous behind. Restore it
// unless no attempt is in flight and the current identity is not a verified
// membership, which is the one sign that the attempt actually finished.
func (t *tunnel) recoverAsideLocked() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	dir := t.directory()
	if _, err := os.Stat(asideDir(dir)); os.IsNotExist(err) {
		return nil
	}
	if t.state == stateStarting || t.state == stateRunning {
		return nil
	}
	view, _, err := t.membershipAt(dir, true)
	if err == nil && view.Verified {
		return dropAside(dir)
	}
	if err := restoreAside(dir); err != nil {
		return errors.Join(errors.New("an interrupted enrollment left the previous identity aside"), err)
	}
	log.Printf("onv-tunnel: an interrupted enrollment was rolled back to the previous identity")
	return nil
}
