package main

// **Machine mode: the tunnel as a marketplace machine's network (W3, the
// operator, 4 October 2026).** A Windows machine joins its buyer's private
// network with this daemon rather than with NetBird's own Windows client. A
// Linux machine runs `netbird up --setup-key …` from cloud-init
// (omnuv-provider src/instance.rs, overlay_runcmd); this is the Windows twin.
//
// What differs from the device this daemon was written for:
//
//	device                              machine
//	a person signs in and pairs it      nobody signs in; nobody pairs it
//	joined by the client over IPC       joined by a file on its first-boot drive
//	the first user to act owns it       no owner: administrators, and only them
//	the router port is the owner's      off, and no request turns it on
//	one identity per Core deployment    one identity, bound to the machine's id
//
// **The mode is declared, never inferred.** `<base>/mode` holds `machine`,
// written by the image's install step (machine/install-machine.ps1). Absent,
// the daemon is a device as before; anything else and it refuses to start,
// because a daemon guessing which product it is would be both, badly.
//
// **The contract with the agent: `<base>/machine-join.json`** (version 1).
// On Windows `<base>` is `C:\ProgramData\onv\tunnel`. The agent's Windows
// user-data puts it there with cloudbase-init's `write_files` (`encoding:
// b64`), which logs no content and inherits the directory's SYSTEM and
// Administrators DACL:
//
//	{
//	  "version": 1,
//	  "machine_id": "<the machine's uuid>",
//	  "management_url": "https://api.omnuv.com:8443",
//	  "setup_key": "<the one-time key Core minted, a NetBird UUID>",
//	  "hostname": "<the overlay peer's name>"
//	}
//
// `hostname` is what the Linux path passes as `--hostname`: Core's name when
// it sends one, else `onv-m-<machine id>`, and that default is applied here
// too when the field is empty or absent. Every field is checked before
// anything is spent, and the setup key must have NetBird's own shape (an
// UUID): a placeholder such as `<redacted>` is refused here, loudly, where the
// Linux path once enrolled nothing behind `|| true`.
//
// **The key is never logged and leaves the disk once spent.** The file is
// removed when the join succeeded, and when the overlay refused the key
// (spent, revoked, unknown — none of which waiting fixes). Any other failure
// (no network yet at first boot, the overlay unreachable) keeps it for the
// next attempt, with backoff. What is kept afterwards is `machine.json`: the
// machine's id, the peer name, and a sha256 of the spent key, so a re-sent
// key is recognised as spent rather than tried again — the thousandth
// identical file means nothing.
//
// **Retries cannot mint a second peer.** The identity (WireGuard private key)
// is made once, by NetBird, in `<base>/machine/config.json`, and is never set
// aside the way a device's is: NetBird logs in with it first and registers
// it with the key only if the overlay does not know it
// (client/internal/auth/auth.go, Login), so an attempt that spent the key and
// then failed is finished by the next one without the key.
//
// **Identified by id.** The identity is bound to `machine_id`. A file naming
// another machine — a disk reused, an image that kept what it should not —
// replaces the identity outright: a new machine inherits nothing from an old
// one, whatever it is called.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	netbird "github.com/netbirdio/netbird/client/embed"
)

const (
	modeFileName      = "mode"
	machineJoinName   = "machine-join.json"
	machineDirName    = "machine"
	machineRecordName = "machine.json"
	// How often the loop looks for a join file and at the tunnel. A stat of
	// one file; there is nothing to wait on that could tell us sooner.
	machinePoll = 2 * time.Second
	// The ceiling of the retry backoff: a machine whose overlay is down is
	// asked again at least this often.
	machineRetryCeiling = 60 * time.Second
	maxJoinFileBytes    = 16384
)

var (
	errMachineMode       = errors.New("machine-mode: this machine joins its network from its first-boot drive, and nothing else changes it")
	errMachineNotJoined  = errors.New("this machine has not joined its network yet: waiting for " + machineJoinName)
	errMachineAdminsOnly = errors.New("not-administrator: this machine's network answers administrators only")
	dnsLabel             = regexp.MustCompile(`^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$`)
)

type machineJoin struct {
	Version       int    `json:"version"`
	MachineID     string `json:"machine_id"`
	ManagementURL string `json:"management_url"`
	SetupKey      string `json:"setup_key"`
	Hostname      string `json:"hostname"`
}

type machineRecord struct {
	Version       int       `json:"version"`
	MachineID     string    `json:"machine_id"`
	ManagementURL string    `json:"management_url"`
	PeerName      string    `json:"peer_name"`
	KeyHash       string    `json:"key_hash"`
	IdentityHash  string    `json:"identity_hash"`
	Joined        time.Time `json:"joined"`
}

// What `machine-v1` answers. The key is never in it; whether one waits on
// disk is.
type machineView struct {
	Mode          string     `json:"mode"`
	MachineID     string     `json:"machine_id"`
	PeerName      string     `json:"peer_name"`
	ManagementURL string     `json:"management_url"`
	Joined        *time.Time `json:"joined"`
	JoinPending   bool       `json:"join_pending"`
	Held          bool       `json:"held"`
	Refused       bool       `json:"refused"`
	State         int        `json:"state"`
	Error         string     `json:"error"`
	Address       string     `json:"address"`
	Name          string     `json:"name"`
}

// The peer's name: the agent's, or the whole id in the overlay's own form.
func (j machineJoin) peerName() string {
	if name := strings.TrimSpace(j.Hostname); name != "" {
		return name
	}
	return "onv-m-" + strings.ToLower(j.MachineID)
}

func (j machineJoin) validate() error {
	switch {
	case j.Version != 1:
		return fmt.Errorf("version %d is not one this daemon reads (1)", j.Version)
	case !uuidText.MatchString(j.MachineID):
		return errors.New("machine_id is not a uuid")
	case !origin(j.ManagementURL):
		return errors.New("management_url is not an https origin")
	case !uuidText.MatchString(j.SetupKey):
		return errors.New("setup_key does not have a setup key's shape")
	case !dnsLabel.MatchString(j.peerName()):
		return errors.New("hostname is not a DNS label")
	}
	return nil
}

// Whether `<base>/mode` declares this daemon a machine's. Absent is a
// device; anything but the two words is an error, never a guess.
func readMode(base string) (bool, error) {
	raw, err := os.ReadFile(filepath.Join(base, modeFileName))
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("read the tunnel's mode: %w", err)
	}
	switch strings.TrimSpace(strings.TrimPrefix(string(raw), "\uFEFF")) {
	case "machine":
		return true, nil
	case "device", "":
		return false, nil
	}
	return false, fmt.Errorf("%s says neither machine nor device", filepath.Join(base, modeFileName))
}

func (t *tunnel) joinPath() string { return filepath.Join(t.base(), machineJoinName) }

// The join file, checked; nil when there is none. An error means the file is
// there and will never work, and its message never quotes the file.
func readMachineJoin(path string) (*machineJoin, error) {
	f, err := os.Open(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("open: %w", err)
	}
	defer f.Close()
	raw, err := io.ReadAll(io.LimitReader(f, maxJoinFileBytes+1))
	if err != nil {
		return nil, fmt.Errorf("read: %w", err)
	}
	if len(raw) > maxJoinFileBytes {
		return nil, errors.New("larger than a join file can be")
	}
	// cloudbase-init writes bytes as given; a BOM from a hand-written test
	// file is not worth refusing a machine over.
	raw = []byte(strings.TrimPrefix(string(raw), "\uFEFF"))
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.DisallowUnknownFields()
	var join machineJoin
	if err := decoder.Decode(&join); err != nil {
		// json's messages name a field or an offset, never a value.
		var syntax *json.SyntaxError
		if errors.As(err, &syntax) {
			return nil, fmt.Errorf("not JSON (at byte %d)", syntax.Offset)
		}
		return nil, fmt.Errorf("not a join file: %s", strings.TrimPrefix(err.Error(), "json: "))
	}
	if decoder.Decode(new(any)) != io.EOF {
		return nil, errors.New("more than one JSON object")
	}
	if err := join.validate(); err != nil {
		return nil, err
	}
	return &join, nil
}

func readMachineRecord(dir string) (*machineRecord, error) {
	raw, err := os.ReadFile(filepath.Join(dir, machineRecordName))
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("read the machine record: %w", err)
	}
	var record machineRecord
	if json.Unmarshal(raw, &record) != nil || record.Version != 1 || !uuidText.MatchString(record.MachineID) ||
		!origin(record.ManagementURL) || record.IdentityHash == "" || record.KeyHash == "" {
		return nil, errors.New("the machine record is unreadable")
	}
	return &record, nil
}

// Removes the join file only while it still carries the key that was used, so
// a newer file written meanwhile is not lost with it.
func removeSpentJoin(path, keyHash string) error {
	join, err := readMachineJoin(path)
	if join == nil && err == nil {
		return nil
	}
	if join != nil && hashText(join.SetupKey) != keyHash {
		return nil
	}
	return removeIfPresent(path)
}

// Another machine's identity goes before this one's is made.
func clearMachineIdentity(dir string) error {
	for _, name := range []string{"config.json", "state.json", machineRecordName} {
		if err := removeIfPresent(filepath.Join(dir, name)); err != nil {
			return err
		}
	}
	return nil
}

// Caller holds operations. With a join, the key is offered (NetBird spends it
// only if the identity is new to the overlay); without one, the identity the
// record is bound to resumes.
func (t *tunnel) machineStartLocked(join *machineJoin) error {
	t.mu.Lock()
	active := t.state == stateStarting || t.state == stateRunning
	t.mu.Unlock()
	if join == nil && active {
		return nil
	}
	if err := t.stopLocked(); err != nil {
		return err
	}
	dir := t.directory()
	if err := secureDir(dir); err != nil {
		t.setFailed(err)
		return err
	}
	configPath, statePath := filepath.Join(dir, "config.json"), filepath.Join(dir, "state.json")
	record, err := readMachineRecord(dir)
	if err != nil {
		t.setFailed(err)
		return err
	}
	opts := netbird.Options{ConfigPath: configPath, StatePath: statePath, NoUserspace: true}
	keyHash := ""
	if join != nil {
		if record != nil && record.MachineID != join.MachineID {
			log.Printf("onv-tunnel: machine: the identity of machine %s is replaced by machine %s's", record.MachineID, join.MachineID)
			if err := clearMachineIdentity(dir); err != nil {
				t.setFailed(err)
				return err
			}
			record = nil
		}
		opts.DeviceName, opts.ManagementURL, opts.SetupKey = join.peerName(), join.ManagementURL, join.SetupKey
		keyHash = hashText(join.SetupKey)
	} else {
		if record == nil {
			t.mu.Lock()
			t.state, t.lastError = stateStopped, errMachineNotJoined.Error()
			t.mu.Unlock()
			return errMachineNotJoined
		}
		identity, err := readIdentity(configPath)
		if err == nil && (identity == "" || hashText(identity) != record.IdentityHash) {
			err = errors.New("the machine's identity is not the one its record was made for")
		}
		if err != nil {
			t.setFailed(err)
			return err
		}
		opts.DeviceName, opts.ManagementURL, opts.PrivateKey = record.PeerName, record.ManagementURL, identity
	}
	// No request can turn the router port on for a machine: started as a
	// keyed identity, which no consent has ever been given to.
	t.preparePortmapLocked(dir, "", true)
	factory := t.factory
	if factory == nil {
		factory = func(options netbird.Options) (tunnelClient, error) {
			client, err := netbird.New(options)
			if err != nil {
				return nil, err
			}
			return &embeddedClient{Client: client}, nil
		}
	}
	log.Printf("onv-tunnel: machine: starting key=%t peer=%s", join != nil, opts.DeviceName)
	client, err := factory(opts)
	if err != nil {
		t.setFailed(err)
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Minute)
	t.mu.Lock()
	t.generation++
	generation := t.generation
	done := make(chan struct{})
	t.client, t.cancel, t.done = client, cancel, done
	t.state, t.lastError = stateStarting, ""
	t.mu.Unlock()
	joinPath := t.joinPath()
	go func() {
		defer close(done)
		defer cancel()
		err := client.Start(ctx)
		if err == nil {
			err = ctx.Err()
		}
		var identity string
		if err == nil {
			identity, err = client.Identity()
			if err == nil && identity == "" {
				err = errors.New("successful tunnel start did not report its identity")
			}
		}
		if err == nil {
			if stored, readErr := readIdentity(configPath); readErr != nil || stored != identity {
				err = errors.Join(errors.New("the stored identity differs from the running tunnel's"), readErr)
			}
		}
		if err == nil && join != nil {
			err = writeJSONAtomic(filepath.Join(dir, machineRecordName), machineRecord{
				Version: 1, MachineID: join.MachineID, ManagementURL: join.ManagementURL, PeerName: join.peerName(),
				KeyHash: keyHash, IdentityHash: hashText(identity), Joined: time.Now().UTC(),
			})
		}
		t.mu.Lock()
		if generation != t.generation {
			t.mu.Unlock()
			return
		}
		if err == nil {
			t.state, t.machineFailures, t.machineRefused = stateRunning, 0, false
			t.mu.Unlock()
			if join != nil {
				if rmErr := removeSpentJoin(joinPath, keyHash); rmErr != nil {
					log.Printf("onv-tunnel: machine: the spent join file could not be removed: %v", rmErr)
				}
				log.Printf("onv-tunnel: machine %s joined its network as %s; its key is spent and removed", join.MachineID, join.peerName())
			} else {
				log.Printf("onv-tunnel: machine: running")
			}
			return
		}
		err = machineRefusal(err)
		refused := strings.HasPrefix(err.Error(), "unauthorized:")
		t.state, t.lastError = stateFailed, err.Error()
		t.machineFailures++
		t.machineRefused = refused
		t.mu.Unlock()
		cleanup, cancelCleanup := context.WithTimeout(context.Background(), 30*time.Second)
		stopErr := client.Stop(cleanup)
		cancelCleanup()
		if stopErr != nil {
			t.mu.Lock()
			if generation == t.generation {
				t.lastError += "; tunnel cleanup failed: " + stopErr.Error()
			}
			t.mu.Unlock()
		}
		if refused && join != nil {
			// A key the overlay refused never works later: it goes now, and
			// the machine waits for a new one.
			if rmErr := removeSpentJoin(joinPath, keyHash); rmErr != nil {
				log.Printf("onv-tunnel: machine: the refused join file could not be removed: %v", rmErr)
			}
		}
		log.Printf("onv-tunnel: machine: start failed (refused=%t): %v", refused, err)
	}()
	return nil
}

// **What the overlay says to a key it will not take**, measured against
// NetBird 0.78.1's own management server (e2e/machine, 4 October 2026): a
// spent, revoked, expired or unknown setup key is `NotFound` (or, racing
// another registration, `PreconditionFailed`) "couldn't add peer: setup key
// is invalid" (management/server/peer.go:713, 717, 923) - not the
// PermissionDenied classify() was written for, so it read as a retry, and
// NetBird's own Login had already retried it for two minutes. For a machine
// it is final: the key goes, and the machine waits for a new one.
//
// Not added to classify() itself: a device's app reads "unauthorized:" as
// "no longer a member" unless the sentence says "setup-key"
// (app/omnuv/tunnel.cpp), so the device half is queued, not changed here.
func machineRefusal(err error) error {
	err = classify(err)
	if err != nil && !strings.HasPrefix(err.Error(), "unauthorized:") &&
		strings.Contains(strings.ToLower(err.Error()), "setup key is invalid") {
		return fmt.Errorf("unauthorized: %w", err)
	}
	return err
}

func writeJSONAtomic(path string, value any) error {
	raw, err := json.Marshal(value)
	if err != nil {
		return err
	}
	return writeFileAtomic(path, raw)
}

// The backoff before the n-th consecutive retry: the poll, doubled per
// failure, to the ceiling.
func machineBackoff(failures int) time.Duration {
	delay := machinePoll
	for i := 1; i < failures && delay < machineRetryCeiling; i++ {
		delay *= 2
	}
	if delay > machineRetryCeiling {
		delay = machineRetryCeiling
	}
	return delay
}

// One look: a join file to spend, or a joined tunnel to bring back. Returns
// what it did, for the tests and the log; "" when nothing was due.
func (t *tunnel) machineStep(now time.Time) string {
	t.operations.Lock()
	defer t.operations.Unlock()
	t.mu.Lock()
	state, held, refused, nextTry := t.state, t.machineHeld, t.machineRefused, t.machineNextTry
	dir := t.directory()
	t.mu.Unlock()
	if state == stateStarting {
		return ""
	}
	join, err := readMachineJoin(t.joinPath())
	if err != nil {
		// It will never work; it may hold a key. Neither is a reason to keep it.
		log.Printf("onv-tunnel: machine: %s refused and removed: %v", machineJoinName, err)
		if rmErr := removeIfPresent(t.joinPath()); rmErr != nil {
			log.Printf("onv-tunnel: machine: could not remove %s: %v", machineJoinName, rmErr)
		}
		t.mu.Lock()
		if t.state != stateRunning {
			t.state = stateFailed
		}
		t.lastError = machineJoinName + " refused: " + err.Error()
		t.mu.Unlock()
		return "refused-file"
	}
	record, recordErr := readMachineRecord(dir)
	if join != nil && recordErr == nil && record != nil && record.MachineID == join.MachineID && record.KeyHash == hashText(join.SetupKey) {
		// The same key again: spent, and nothing to do with it.
		if rmErr := removeIfPresent(t.joinPath()); rmErr != nil {
			log.Printf("onv-tunnel: machine: could not remove a spent %s: %v", machineJoinName, rmErr)
		}
		log.Printf("onv-tunnel: machine: %s carried the key already spent; removed", machineJoinName)
		join = nil
	}
	if join != nil {
		// A new key is a new instruction: it clears a hold and a refusal. It
		// waits out the backoff only after a failed attempt with it.
		if state == stateFailed && now.Before(nextTry) {
			return ""
		}
		t.mu.Lock()
		t.machineHeld, t.machineRefused = false, false
		t.machineNextTry = now.Add(machineBackoff(t.machineFailures + 1))
		t.mu.Unlock()
		if err := t.machineStartLocked(join); err != nil {
			log.Printf("onv-tunnel: machine: could not start: %v", err)
		}
		return "join"
	}
	if held || refused || state == stateRunning || record == nil || now.Before(nextTry) {
		return ""
	}
	t.mu.Lock()
	t.machineNextTry = now.Add(machineBackoff(t.machineFailures + 1))
	t.mu.Unlock()
	if err := t.machineStartLocked(nil); err != nil {
		log.Printf("onv-tunnel: machine: could not resume: %v", err)
	}
	return "resume"
}

// The service's loop in machine mode, for the life of the process.
func (t *tunnel) machineLoop(ctx context.Context) {
	log.Printf("onv-tunnel: machine mode: waiting on %s", t.joinPath())
	for {
		t.machineStep(time.Now())
		select {
		case <-ctx.Done():
			return
		case <-time.After(machinePoll):
		}
	}
}

// After the service starts: a machine's loop, or a device's resume.
func (t *tunnel) begin() {
	if t.machine {
		go t.machineLoop(context.Background())
		return
	}
	go func() {
		if err := t.start("", ""); err != nil {
			log.Printf("onv-tunnel: nothing to resume: %v", err)
		}
	}()
}

func (t *tunnel) machineSnapshot() (string, error) {
	t.mu.Lock()
	dir := t.directory()
	view := machineView{Mode: "machine", Held: t.machineHeld, Refused: t.machineRefused, State: t.state, Error: t.lastError}
	t.mu.Unlock()
	record, err := readMachineRecord(dir)
	if err != nil {
		return "", err
	}
	if record != nil {
		joined := record.Joined
		view.MachineID, view.PeerName, view.ManagementURL, view.Joined = record.MachineID, record.PeerName, record.ManagementURL, &joined
	}
	if _, statErr := os.Stat(t.joinPath()); statErr == nil {
		view.JoinPending = true
	}
	view.Address, view.Name = t.address()
	raw, err := json.Marshal(view)
	if err != nil {
		return "", err
	}
	return string(raw), nil
}

// The IPC in machine mode. `state` is anybody's, as on a device; everything
// else is an administrator's (SYSTEM, the guest agent's commands, an elevated
// shell), and nobody ever becomes the owner. What would change the machine's
// membership is refused to everyone: that is the first-boot drive's alone.
//
//	state                     anybody
//	machine-v1                the machine's id, peer, join state (never the key)
//	peers-v1 [address]        as on a device: the gaming play reads the path
//	portmap-v1                the read; on|off is refused (off, by W3)
//	stop                      held down until resume or a new join file
//	resume                    brought back up with the identity it has
//	anything else             err machine-mode
func machineAnswer(t *tunnel, who caller, line string) string {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return "err empty request"
	}
	if fields[0] == "state" {
		return answer(t, line)
	}
	if !who.admin {
		return "err " + errMachineAdminsOnly.Error()
	}
	switch {
	case fields[0] == "machine-v1" && len(fields) == 1:
		raw, err := t.machineSnapshot()
		if err != nil {
			return "err " + err.Error()
		}
		return raw
	case fields[0] == "peers-v1", fields[0] == "portmap-v1" && len(fields) == 1:
		return answer(t, line)
	case fields[0] == "stop" && len(fields) == 1:
		t.operations.Lock()
		t.mu.Lock()
		t.machineHeld = true
		t.mu.Unlock()
		err := t.stopLocked()
		t.operations.Unlock()
		if err != nil {
			return "err " + err.Error()
		}
		log.Printf("onv-tunnel: machine: stopped by an administrator; held until resume")
		return "ok"
	case fields[0] == "resume" && len(fields) == 1:
		t.operations.Lock()
		t.mu.Lock()
		t.machineHeld, t.machineRefused = false, false
		t.mu.Unlock()
		err := t.machineStartLocked(nil)
		t.operations.Unlock()
		if err != nil {
			return "err " + err.Error()
		}
		return "ok"
	}
	return "err " + errMachineMode.Error()
}
