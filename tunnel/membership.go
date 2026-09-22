package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// Public scope is a durable claim bound to this daemon's private identity. A
// legacy config without the binding is deliberately not a verified membership.
type membership struct {
	CoreURL   string `json:"core_url"`
	AccountID string `json:"account_id"`
	ProjectID string `json:"project_id"`
	NetworkID string `json:"network_id"`
	DeviceID  string `json:"device_id"`
}

type membershipRecord struct {
	Version       int        `json:"version"`
	Membership    membership `json:"membership"`
	ManagementURL string     `json:"management_url"`
	Nonce         string     `json:"nonce"`
	IdentityHash  string     `json:"identity_hash"`
	Verified      bool       `json:"verified"`
}

type membershipView struct {
	Version     int         `json:"version"`
	HasIdentity bool        `json:"has_identity"`
	Revision    string      `json:"revision"`
	Membership  *membership `json:"membership"`
	Verified    bool        `json:"verified"`
	Pending     bool        `json:"pending"`
	State       int         `json:"state"`
	Error       string      `json:"error"`
	Address     string      `json:"address"`
	Name        string      `json:"name"`
}

type enrolRequest struct {
	Membership       membership `json:"membership"`
	ManagementURL    string     `json:"management_url"`
	SetupKey         string     `json:"setup_key"`
	ExpectedRevision string     `json:"expected_revision"`
}

type stopRequest struct {
	Membership       membership `json:"membership"`
	ExpectedRevision string     `json:"expected_revision"`
}

var uuidText = regexp.MustCompile(`^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$`)

func origin(raw string) bool {
	u, err := url.Parse(raw)
	return err == nil && u.Scheme == "https" && u.Host != "" && u.User == nil && u.RawQuery == "" && u.Fragment == "" && (u.Path == "" || u.Path == "/")
}

func (m membership) valid() bool {
	return origin(m.CoreURL) && uuidText.MatchString(m.AccountID) && uuidText.MatchString(m.ProjectID) && uuidText.MatchString(m.NetworkID) && uuidText.MatchString(m.DeviceID)
}

func decodeRequest(raw string, result any) error {
	if len(raw) > 16384 {
		return errors.New("membership request exceeds its size limit")
	}
	decoded, err := base64.StdEncoding.Strict().DecodeString(raw)
	if err != nil {
		return errors.New("membership request must be base64 JSON")
	}
	decoder := json.NewDecoder(strings.NewReader(string(decoded)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(result); err != nil {
		return errors.New("membership request has invalid fields")
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return errors.New("membership request must contain one object")
	}
	return nil
}

func hashText(value string) string {
	digest := sha256.Sum256([]byte(value))
	return hex.EncodeToString(digest[:])
}

func (t *tunnel) directory() string {
	if t.dir != "" {
		return t.dir
	}
	return configDir()
}

func readIdentity(path string) (string, error) {
	raw, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("read stored tunnel identity: %w", err)
	}
	var cfg struct{ PrivateKey string }
	if json.Unmarshal(raw, &cfg) != nil || cfg.PrivateKey == "" {
		return "", errors.New("stored tunnel identity is unreadable")
	}
	return cfg.PrivateKey, nil
}

// Caller holds t.mu. Readers never see a new scope bound to an old key: the
// record's fingerprint must agree with the config, including after a crash.
func (t *tunnel) membershipLocked() (membershipView, *membershipRecord, error) {
	view := membershipView{Version: 1, Revision: "empty", State: t.state, Error: t.lastError}
	identity, err := readIdentity(filepath.Join(t.directory(), "config.json"))
	if err != nil {
		return view, nil, err
	}
	view.HasIdentity = identity != ""
	raw, err := os.ReadFile(filepath.Join(t.directory(), "membership.json"))
	if os.IsNotExist(err) {
		if identity != "" {
			view.Revision = hashText("legacy\x00" + identity)
		}
		return view, nil, nil
	}
	if err != nil {
		return view, nil, fmt.Errorf("read membership binding: %w", err)
	}
	var record membershipRecord
	if json.Unmarshal(raw, &record) != nil || record.Version != 1 || !record.Membership.valid() || record.Nonce == "" || !origin(record.ManagementURL) {
		return view, nil, errors.New("stored membership binding is invalid")
	}
	view.Revision = hashText(string(raw) + "\x00" + identity)
	if record.Verified {
		if identity != "" && record.IdentityHash == hashText(identity) {
			view.Verified = true
			view.Membership = &record.Membership
		}
	} else if record.IdentityHash == "" || (identity != "" && record.IdentityHash == hashText(identity)) {
		view.Pending = true
		view.Membership = &record.Membership
	}
	return view, &record, nil
}

func (t *tunnel) membershipSnapshot() (membershipView, error) {
	t.mu.Lock()
	view, _, err := t.membershipLocked()
	client, generation := t.client, t.generation
	t.mu.Unlock()
	if err == nil && client != nil && view.State == stateRunning {
		view.Address, view.Name = client.Address()
		// The library call may block. Never combine its old address with a
		// replacement's state/scope; the UI can retry an explicit failed read.
		t.mu.Lock()
		fresh, _, freshErr := t.membershipLocked()
		if freshErr != nil || generation != t.generation || fresh.State != view.State || fresh.Revision != view.Revision {
			err = errors.New("membership changed while observing tunnel state; retry the snapshot")
		}
		t.mu.Unlock()
	}
	return view, err
}

func (t *tunnel) writeMembership(record membershipRecord) error {
	raw, err := json.Marshal(record)
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(t.directory(), ".membership-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(raw); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(f.Name(), filepath.Join(t.directory(), "membership.json"))
}

func removeIfPresent(path string) error {
	err := os.Remove(path)
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

func newMembership(request enrolRequest) (membershipRecord, error) {
	nonce := make([]byte, 24)
	if _, err := rand.Read(nonce); err != nil {
		return membershipRecord{}, err
	}
	return membershipRecord{Version: 1, Membership: request.Membership, ManagementURL: request.ManagementURL,
		Nonce: hex.EncodeToString(nonce)}, nil
}

func (t *tunnel) resumeMembership(wanted membership) error {
	if !wanted.valid() {
		return errors.New("invalid membership")
	}
	t.operations.Lock()
	defer t.operations.Unlock()
	t.mu.Lock()
	view, record, err := t.membershipLocked()
	t.mu.Unlock()
	if err != nil {
		return err
	}
	if !view.Verified || view.Membership == nil || *view.Membership != wanted {
		return errors.New("membership-mismatch: stored identity is not verified for this scope")
	}
	return t.startLocked(record.ManagementURL, "", record)
}

func (t *tunnel) enrolMembership(request enrolRequest) error {
	if !request.Membership.valid() || !origin(request.ManagementURL) || request.SetupKey == "" || request.ExpectedRevision == "" {
		return errors.New("invalid enrollment request")
	}
	t.operations.Lock()
	defer t.operations.Unlock()
	t.mu.Lock()
	view, _, err := t.membershipLocked()
	if err != nil {
		t.mu.Unlock()
		return err
	}
	if view.Revision != request.ExpectedRevision {
		t.mu.Unlock()
		return errors.New("membership-changed: inspect and confirm the current membership again")
	}
	record, err := newMembership(request)
	if err != nil {
		t.mu.Unlock()
		return err
	}
	t.generation++ // Prevent a prior async completion changing the checked binding.
	t.mu.Unlock()
	return t.startLocked(request.ManagementURL, request.SetupKey, &record)
}

func (t *tunnel) stopMembership(request stopRequest) error {
	if !request.Membership.valid() || request.ExpectedRevision == "" {
		return errors.New("invalid membership stop request")
	}
	t.operations.Lock()
	defer t.operations.Unlock()
	t.mu.Lock()
	view, _, err := t.membershipLocked()
	if err != nil {
		t.mu.Unlock()
		return err
	}
	if view.Revision != request.ExpectedRevision || view.Membership == nil || *view.Membership != request.Membership {
		t.mu.Unlock()
		return errors.New("membership-changed: refusing to stop a different enrollment")
	}
	t.generation++
	t.mu.Unlock()
	return t.stopLocked()
}
