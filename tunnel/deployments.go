package main

import (
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
)

// **One identity per deployment (22 September 2026).** The daemon kept one
// identity, so a device moved between the test Core and production was
// re-enrolled each way: a new key, and the other deployment's peer left
// behind. Each Core origin now has its own directory,
//
//	<base>/deployments/<first 16 hex of sha256(core_url)>/{config,state,membership}.json
//
// and <base>/active names the one in use. Only one runs at a time: the
// embedded client takes one interface and the system's DNS and routes, so
// two would fight. `resume-v1` and `enrol-v1` select the deployment their
// membership names, stopping whatever runs, and a switch back brings the saved
// identity up without a key. `membership-v1 <core_url>` reads a deployment
// without switching to it, so the app can decide to resume rather than
// offering a move.
//
// An identity in the base directory with a membership record naming a Core is
// moved into that Core's directory once. One without a record is left where
// it is, as legacy: it is never guessed into a deployment.

func (t *tunnel) base() string {
	if t.dir != "" {
		return t.dir
	}
	return configDir()
}

func (t *tunnel) dirFor(coreURL string) string {
	if coreURL == "" {
		return t.base()
	}
	return filepath.Join(t.base(), "deployments", hashText(coreURL)[:16])
}

// Caller holds t.mu or operations; idempotent.
func (t *tunnel) loadActive() {
	if t.activeLoaded {
		return
	}
	t.activeLoaded = true
	if raw, err := os.ReadFile(filepath.Join(t.base(), "active")); err == nil {
		if name := strings.TrimSpace(string(raw)); origin(name) {
			t.active = name
			return
		}
	}
	// No active deployment recorded: an identity from before this version, if
	// its record names a Core, moves into that Core's directory.
	_, record, err := t.membershipAt(t.base(), false)
	if err != nil || record == nil {
		return
	}
	if err := t.moveInto(record.Membership.CoreURL); err != nil {
		log.Printf("onv-tunnel: the saved identity stays in the base directory: %v", err)
		return
	}
	t.active = record.Membership.CoreURL
	if err := t.writeActive(); err != nil {
		log.Printf("onv-tunnel: could not record the active deployment: %v", err)
	}
}

func (t *tunnel) moveInto(coreURL string) error {
	target := t.dirFor(coreURL)
	if err := os.MkdirAll(target, 0o700); err != nil {
		return err
	}
	for _, name := range []string{"config.json", "state.json", "membership.json"} {
		from := filepath.Join(t.base(), name)
		if _, err := os.Stat(from); os.IsNotExist(err) {
			continue
		}
		if _, err := os.Stat(filepath.Join(target, name)); err == nil {
			return fmt.Errorf("%s already holds %s", target, name)
		}
		if err := os.Rename(from, filepath.Join(target, name)); err != nil {
			return err
		}
	}
	return nil
}

func (t *tunnel) writeActive() error {
	f, err := os.CreateTemp(t.base(), ".active-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.WriteString(t.active + "\n"); err == nil {
		err = f.Sync()
	}
	if closeErr := f.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(f.Name(), filepath.Join(t.base(), "active"))
}

// Make coreURL the deployment in use, stopping another that runs. Caller holds
// operations. A failed stop leaves the previous deployment active and running.
func (t *tunnel) selectLocked(coreURL string) error {
	if !origin(coreURL) {
		return errors.New("invalid deployment")
	}
	t.mu.Lock()
	t.loadActive()
	same := t.active == coreURL
	t.mu.Unlock()
	if same {
		return nil
	}
	if err := t.stopLocked(); err != nil {
		return err
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	previous := t.active
	t.active = coreURL
	if err := os.MkdirAll(t.base(), 0o700); err != nil {
		t.active = previous
		return err
	}
	if err := t.writeActive(); err != nil {
		t.active = previous
		return err
	}
	return nil
}

// A deployment's membership, read without switching to it.
func (t *tunnel) membershipOf(coreURL string) (membershipView, error) {
	if !origin(coreURL) {
		return membershipView{}, errors.New("membership-v1 takes a Core https origin")
	}
	t.mu.Lock()
	t.loadActive()
	active := t.active == coreURL
	t.mu.Unlock()
	if active {
		return t.membershipSnapshot()
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	view, _, err := t.membershipAt(t.dirFor(coreURL), false)
	return view, err
}
