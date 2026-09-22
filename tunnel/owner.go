package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// **Who may change this machine's network (H4, 22 September 2026).** The pipe
// grants every authenticated user read and write, because the desktop client
// runs as the person logged in and must be able to ask for a join. So the
// socket's permissions were the whole access control, and on a machine with
// two accounts either could stop the tunnel, move it onto a network whose key
// it held, or read the other's account and project ids.
//
// Each request now carries its caller (peer_*.go), and:
//
//	administrator, SYSTEM, root   may do anything: the installer and the service
//	the owner                     may do anything
//	nobody owns it yet            anyone may, and the first non-administrator
//	                              whose change is accepted becomes the owner
//	anybody else                  may ask `state`, and nothing more
//
// The owner is a SID on Windows and a uid elsewhere, kept in <base>/owner.
// Only an administrator can move it, by removing that file.

// A caller whose identity could not be read has no id: it never becomes the
// owner, and it may act only while nobody owns the tunnel.
type caller struct {
	id    string
	admin bool
}

var errNotOwner = errors.New("not-owner: this device's network belongs to another user on this machine")

func (t *tunnel) ownerPath() string { return filepath.Join(t.base(), "owner") }

// Caller holds t.mu.
func (t *tunnel) ownerLocked() (string, error) {
	raw, err := os.ReadFile(t.ownerPath())
	if os.IsNotExist(err) {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("read the tunnel's owner: %w", err)
	}
	return strings.TrimSpace(string(raw)), nil
}

func (t *tunnel) mayAct(who caller) error {
	if who.admin {
		return nil
	}
	t.mu.Lock()
	owner, err := t.ownerLocked()
	t.mu.Unlock()
	if err != nil {
		return err
	}
	if owner == "" || (who.id != "" && owner == who.id) {
		return nil
	}
	return errNotOwner
}

// After a change the caller was allowed to make: a non-administrator with a
// known id claims an unowned tunnel. Never replaces an owner.
func (t *tunnel) claim(who caller) error {
	if who.admin || who.id == "" {
		return nil
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	owner, err := t.ownerLocked()
	if err != nil || owner != "" {
		return err
	}
	if err := os.MkdirAll(t.base(), 0o700); err != nil {
		return err
	}
	f, err := os.CreateTemp(t.base(), ".owner-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.WriteString(who.id + "\n"); err == nil {
		err = f.Sync()
	}
	if closeErr := f.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(f.Name(), t.ownerPath())
}
