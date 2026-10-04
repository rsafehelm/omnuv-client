package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	netbird "github.com/netbirdio/netbird/client/embed"
)

const (
	machineA = "6f1d2c3b-4a59-4e8f-9a0b-1c2d3e4f5a6b"
	machineB = "0b9a8c7d-6e5f-4a3b-8c2d-1e0f9a8b7c6d"
	keyOne   = "0E38B183-B8B6-45CE-B93B-2EF63F3D14E4"
	keyTwo   = "7A1C2B3D-4E5F-4061-8273-94A5B6C7D8E9"
	mgmtURL  = "https://api.omnuv.com:8443"
)

// A stand-in for NetBird with its one behaviour that machine mode relies on:
// the identity is made once, in config.json, and a later start with a key
// reuses it rather than making another.
type machineFixture struct {
	mu      sync.Mutex
	calls   []netbird.Options
	nat     []string
	fail    []error // the next starts' results, in order; nil when exhausted
	made    int
	stopped int
}

func (f *machineFixture) factory(opts netbird.Options) (tunnelClient, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, opts)
	f.nat = append(f.nat, os.Getenv(natMapperEnv))
	identity := opts.PrivateKey
	if identity == "" {
		existing, err := readIdentity(opts.ConfigPath)
		if err != nil {
			return nil, err
		}
		if existing == "" {
			f.made++
			existing = fmt.Sprintf("fixture-machine-identity-%d", f.made)
			if err := writeIdentity(opts.ConfigPath, existing); err != nil {
				return nil, err
			}
		}
		identity = existing
	}
	var result error
	if len(f.fail) > 0 {
		result, f.fail = f.fail[0], f.fail[1:]
	}
	return &fakeClient{
		identity: identity,
		start:    func(context.Context) error { return result },
		stop: func(context.Context) error {
			f.mu.Lock()
			f.stopped++
			f.mu.Unlock()
			return nil
		},
	}, nil
}

func (f *machineFixture) last() netbird.Options {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls[len(f.calls)-1]
}

func (f *machineFixture) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.calls)
}

func machineTunnel(t *testing.T, dir string) (*tunnel, *machineFixture) {
	t.Helper()
	fx := &machineFixture{}
	tn := &tunnel{dir: dir, machine: true, factory: fx.factory}
	t.Cleanup(func() { _ = tn.stop() })
	return tn, fx
}

func writeJoin(t *testing.T, tn *tunnel, join map[string]any) {
	t.Helper()
	raw, err := json.Marshal(join)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(tn.joinPath(), raw, 0o600); err != nil {
		t.Fatal(err)
	}
}

func joinOf(machine, key string) map[string]any {
	return map[string]any{"version": 1, "machine_id": machine, "management_url": mgmtURL, "setup_key": key}
}

// The log, for the length of a test: what the daemon wrote is read back to
// prove a key never reached it.
func captureLog(t *testing.T) *bytes.Buffer {
	t.Helper()
	var buf bytes.Buffer
	var mu sync.Mutex
	log.SetOutput(writerFunc(func(p []byte) (int, error) {
		mu.Lock()
		defer mu.Unlock()
		return buf.Write(p)
	}))
	t.Cleanup(func() { log.SetOutput(os.Stderr) })
	return &buf
}

type writerFunc func([]byte) (int, error)

func (w writerFunc) Write(p []byte) (int, error) { return w(p) }

func stateOf(tn *tunnel) (int, string) { return tn.snapshot() }

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func TestModeIsDeclaredNeverGuessed(t *testing.T) {
	for _, c := range []struct {
		content string
		want    bool
		fails   bool
	}{
		{"", false, false},
		{"machine\n", true, false},
		{"\uFEFFmachine\r\n", true, false},
		{"device", false, false},
		{"laptop", false, true},
		{"Machine", false, true},
	} {
		dir := t.TempDir()
		if c.content != "" {
			if err := os.WriteFile(filepath.Join(dir, modeFileName), []byte(c.content), 0o600); err != nil {
				t.Fatal(err)
			}
		}
		got, err := readMode(dir)
		if (err != nil) != c.fails || got != c.want {
			t.Errorf("mode %q: got %v, %v", c.content, got, err)
		}
	}
}

func TestJoinFileIsCheckedBeforeAnythingIsSpent(t *testing.T) {
	good := joinOf(machineA, keyOne)
	with := func(k string, v any) map[string]any {
		m := map[string]any{}
		for key, value := range good {
			m[key] = value
		}
		if v == nil {
			delete(m, k)
		} else {
			m[k] = v
		}
		return m
	}
	dir := t.TempDir()
	path := filepath.Join(dir, machineJoinName)
	check := func(name string, raw []byte, ok bool) {
		t.Helper()
		if err := os.WriteFile(path, raw, 0o600); err != nil {
			t.Fatal(err)
		}
		join, err := readMachineJoin(path)
		if ok != (err == nil && join != nil) {
			t.Errorf("%s: join=%v err=%v", name, join, err)
		}
		if err != nil && strings.Contains(err.Error(), keyOne) {
			t.Errorf("%s: the refusal quotes the key: %v", name, err)
		}
	}
	encode := func(m map[string]any) []byte { raw, _ := json.Marshal(m); return raw }
	check("good", encode(good), true)
	check("a peer name from Core", encode(with("hostname", "gaming-rig-1a2b3c4d")), true)
	check("a BOM", append([]byte("\uFEFF"), encode(good)...), true)
	check("version 2", encode(with("version", 2)), false)
	check("no version", encode(with("version", nil)), false)
	check("a name for an id", encode(with("machine_id", "gaming-rig")), false)
	check("eight hex digits for an id", encode(with("machine_id", "6f1d2c3b")), false)
	check("http to a remote overlay", encode(with("management_url", "http://api.omnuv.com:8443")), false)
	check("a path in the url", encode(with("management_url", mgmtURL+"/x")), false)
	check("the redacted placeholder", encode(with("setup_key", "<redacted>")), false)
	check("an empty key", encode(with("setup_key", "")), false)
	check("a hostname that is no label", encode(with("hostname", "a b")), false)
	check("an unknown field", encode(with("console_password", "x")), false)
	check("two objects", append(encode(good), encode(good)...), false)
	check("not JSON, with the key in it", []byte("setup_key="+keyOne), false)
	check("too large", append(encode(good), bytes.Repeat([]byte(" "), maxJoinFileBytes)...), false)
	if join, err := readMachineJoin(filepath.Join(dir, "absent.json")); join != nil || err != nil {
		t.Errorf("an absent file is no join and no error: %v %v", join, err)
	}
}

func TestMachineJoinsOnceSpendsTheKeyAndLogsItNowhere(t *testing.T) {
	logged := captureLog(t)
	tn, fx := machineTunnel(t, t.TempDir())
	if got := tn.machineStep(time.Now()); got != "" {
		t.Fatalf("with no file and no identity there is nothing to do: %q", got)
	}
	writeJoin(t, tn, joinOf(machineA, keyOne))
	if got := tn.machineStep(time.Now()); got != "join" {
		t.Fatalf("step: %q", got)
	}
	finish(t, tn)
	if state, why := stateOf(tn); state != stateRunning {
		t.Fatalf("state %d: %s", state, why)
	}
	opts := fx.last()
	if opts.SetupKey != keyOne || opts.PrivateKey != "" || opts.ManagementURL != mgmtURL || !opts.NoUserspace {
		t.Fatalf("options: key=%t private=%t url=%s userspace-off=%t", opts.SetupKey == keyOne, opts.PrivateKey != "", opts.ManagementURL, opts.NoUserspace)
	}
	if opts.DeviceName != "onv-m-"+machineA {
		t.Fatalf("the peer is named by the machine's whole id: %q", opts.DeviceName)
	}
	if fx.nat[0] != "true" {
		t.Fatalf("NB_DISABLE_NAT_MAPPER=%q: a machine never asks a router", fx.nat[0])
	}
	if exists(tn.joinPath()) {
		t.Fatal("the spent key is still on disk")
	}
	record, err := readMachineRecord(filepath.Join(tn.dir, machineDirName))
	if err != nil || record == nil {
		t.Fatalf("record: %v %v", record, err)
	}
	if record.MachineID != machineA || record.PeerName != "onv-m-"+machineA || record.KeyHash != hashText(keyOne) ||
		record.IdentityHash != hashText("fixture-machine-identity-1") {
		t.Fatalf("record: %+v", record)
	}
	if exists(filepath.Join(tn.dir, "owner")) || exists(filepath.Join(tn.dir, "active")) || exists(filepath.Join(tn.dir, "deployments")) {
		t.Fatal("a machine has no owner and no deployment")
	}
	walk := func() string {
		var all strings.Builder
		_ = filepath.Walk(tn.dir, func(path string, info os.FileInfo, err error) error {
			if err == nil && !info.IsDir() {
				raw, _ := os.ReadFile(path)
				all.Write(raw)
			}
			return nil
		})
		return all.String()
	}
	if strings.Contains(walk(), keyOne) {
		t.Fatal("the key survives in a file")
	}
	if strings.Contains(logged.String(), keyOne) {
		t.Fatal("the key reached the log")
	}
	if !strings.Contains(logged.String(), "joined its network as onv-m-"+machineA) {
		t.Fatalf("the join is not said: %s", logged.String())
	}

	// The same key again (cloudbase-init's per-instance plugins rerun, or a
	// copy the agent sent twice): spent, removed, nothing started.
	writeJoin(t, tn, joinOf(machineA, keyOne))
	calls := fx.count()
	if got := tn.machineStep(time.Now()); got != "" {
		t.Fatalf("a spent key started something: %q", got)
	}
	if fx.count() != calls || exists(tn.joinPath()) {
		t.Fatal("a spent key was offered again, or left on disk")
	}
}

func TestMachineResumesItsOwnIdentityAfterARestart(t *testing.T) {
	dir := t.TempDir()
	first, _ := machineTunnel(t, dir)
	writeJoin(t, first, map[string]any{"version": 1, "machine_id": machineA, "management_url": mgmtURL, "setup_key": keyOne, "hostname": "gaming-1a2b3c4d"})
	first.machineStep(time.Now())
	finish(t, first)
	if err := first.stop(); err != nil {
		t.Fatal(err)
	}

	again, fx := machineTunnel(t, dir)
	if got := again.machineStep(time.Now()); got != "resume" {
		t.Fatalf("step: %q", got)
	}
	finish(t, again)
	opts := fx.last()
	if opts.SetupKey != "" || opts.PrivateKey != "fixture-machine-identity-1" || opts.DeviceName != "gaming-1a2b3c4d" || opts.ManagementURL != mgmtURL {
		t.Fatalf("resume: key=%t private=%q name=%q", opts.SetupKey != "", opts.PrivateKey, opts.DeviceName)
	}
	if state, why := stateOf(again); state != stateRunning {
		t.Fatalf("state %d: %s", state, why)
	}
	if got := again.machineStep(time.Now()); got != "" {
		t.Fatalf("a running machine was started again: %q", got)
	}
}

func TestMachineRetriesWithTheSameIdentityAndKeepsTheKeyUntilThen(t *testing.T) {
	tn, fx := machineTunnel(t, t.TempDir())
	fx.fail = []error{errors.New("rpc error: code = Unavailable desc = connection refused")}
	writeJoin(t, tn, joinOf(machineA, keyOne))
	now := time.Now()
	tn.machineStep(now)
	finish(t, tn)
	if state, _ := stateOf(tn); state != stateFailed {
		t.Fatalf("state %d", state)
	}
	if !exists(tn.joinPath()) {
		t.Fatal("a key the overlay never answered was removed")
	}
	if got := tn.machineStep(now.Add(time.Second)); got != "" {
		t.Fatalf("retried inside the backoff: %q", got)
	}
	if got := tn.machineStep(now.Add(machineBackoff(1) + time.Second)); got != "join" {
		t.Fatalf("no retry after the backoff: %q", got)
	}
	finish(t, tn)
	if state, why := stateOf(tn); state != stateRunning {
		t.Fatalf("state %d: %s", state, why)
	}
	if fx.made != 1 {
		t.Fatalf("%d identities made: a retry must reuse the first, or a spent key mints nothing", fx.made)
	}
	if exists(tn.joinPath()) {
		t.Fatal("the key outlived the join")
	}
}

func TestARefusedKeyGoesAndTheMachineWaitsForANewOne(t *testing.T) {
	tn, fx := machineTunnel(t, t.TempDir())
	fx.fail = []error{errors.New("rpc error: code = PermissionDenied desc = invalid setup-key or no sso information provided")}
	writeJoin(t, tn, joinOf(machineA, keyOne))
	now := time.Now()
	tn.machineStep(now)
	finish(t, tn)
	state, why := stateOf(tn)
	if state != stateFailed || !strings.HasPrefix(why, "unauthorized:") {
		t.Fatalf("state %d: %s", state, why)
	}
	if exists(tn.joinPath()) {
		t.Fatal("a refused key was kept")
	}
	if got := tn.machineStep(now.Add(time.Hour)); got != "" {
		t.Fatalf("a refused machine tried again by itself: %q", got)
	}
	writeJoin(t, tn, joinOf(machineA, keyTwo))
	if got := tn.machineStep(now.Add(2 * time.Hour)); got != "join" {
		t.Fatalf("a new key was not tried: %q", got)
	}
	finish(t, tn)
	if state, why := stateOf(tn); state != stateRunning {
		t.Fatalf("state %d: %s", state, why)
	}
	if fx.last().SetupKey != keyTwo {
		t.Fatal("the new key was not the one offered")
	}
}

// The sentence NetBird 0.78.1's management answers a spent key with, as the
// e2e met it: final for a machine, after the library's own two minutes.
func TestASpentKeyInNetBirdsWordsIsFinal(t *testing.T) {
	tn, fx := machineTunnel(t, t.TempDir())
	fx.fail = []error{errors.New("login: rpc error: code = NotFound desc = couldn't add peer: setup key is invalid")}
	writeJoin(t, tn, joinOf(machineA, keyOne))
	tn.machineStep(time.Now())
	finish(t, tn)
	if state, why := stateOf(tn); state != stateFailed || !strings.HasPrefix(why, "unauthorized:") || exists(tn.joinPath()) {
		t.Fatalf("state %d: %s; file kept %t", state, why, exists(tn.joinPath()))
	}
	if got := classify(errors.New("couldn't add peer: setup key is invalid")).Error(); strings.HasPrefix(got, "unauthorized:") {
		t.Fatalf("the device's classification moved with the machine's: %s", got)
	}
}

func TestAnotherMachinesIdentityIsNeverInherited(t *testing.T) {
	tn, fx := machineTunnel(t, t.TempDir())
	writeJoin(t, tn, joinOf(machineA, keyOne))
	tn.machineStep(time.Now())
	finish(t, tn)
	writeJoin(t, tn, joinOf(machineB, keyTwo))
	if got := tn.machineStep(time.Now()); got != "join" {
		t.Fatalf("step: %q", got)
	}
	finish(t, tn)
	record, _ := readMachineRecord(filepath.Join(tn.dir, machineDirName))
	if fx.made != 2 || record == nil || record.MachineID != machineB || record.IdentityHash != hashText("fixture-machine-identity-2") {
		t.Fatalf("made %d, record %+v", fx.made, record)
	}
	if fx.last().DeviceName != "onv-m-"+machineB {
		t.Fatalf("peer %q", fx.last().DeviceName)
	}
}

func TestAJoinFileThatCanNeverWorkIsRemovedAndSaid(t *testing.T) {
	logged := captureLog(t)
	tn, fx := machineTunnel(t, t.TempDir())
	bad := joinOf(machineA, keyOne)
	bad["management_url"] = "http://api.omnuv.com"
	writeJoin(t, tn, bad)
	if got := tn.machineStep(time.Now()); got != "refused-file" {
		t.Fatalf("step: %q", got)
	}
	state, why := stateOf(tn)
	if state != stateFailed || !strings.Contains(why, "management_url") {
		t.Fatalf("state %d: %s", state, why)
	}
	if exists(tn.joinPath()) || fx.count() != 0 {
		t.Fatal("a refused file was kept, or spent")
	}
	if strings.Contains(logged.String(), keyOne) || strings.Contains(why, keyOne) {
		t.Fatal("the key was said")
	}
}

func TestMachineIPCIsAdministratorsAndChangesNoMembership(t *testing.T) {
	tn, _ := machineTunnel(t, t.TempDir())
	writeJoin(t, tn, joinOf(machineA, keyOne))
	tn.machineStep(time.Now())
	finish(t, tn)

	user := caller{id: "S-1-5-21-1000"}
	admin := caller{id: "S-1-5-18", admin: true}
	if got := answerFor(tn, user, "state"); !strings.HasPrefix(got, "state 2 ") {
		t.Fatalf("state is anybody's: %q", got)
	}
	for _, line := range []string{"peers-v1", "machine-v1", "stop", "resume", "portmap-v1", "enrol " + mgmtURL + " " + keyTwo} {
		if got := answerFor(tn, user, line); !strings.HasPrefix(got, "err not-administrator") {
			t.Errorf("%q from a user: %q", line, got)
		}
	}
	if exists(filepath.Join(tn.dir, "owner")) {
		t.Fatal("a user became a machine's owner")
	}
	for _, line := range []string{"enrol " + mgmtURL + " " + keyTwo, "enrol-v1 e30=", "resume-v1 e30=", "stop-v1 e30=",
		"membership-v1", "portmap-v1 on", "portmap-v1 off", "unheard-of"} {
		if got := answerFor(tn, admin, line); !strings.HasPrefix(got, "err machine-mode") {
			t.Errorf("%q from an administrator: %q", line, got)
		}
	}
	var peers peersView
	if err := json.Unmarshal([]byte(answerFor(tn, admin, "peers-v1")), &peers); err != nil || peers.State != stateRunning {
		t.Fatalf("peers-v1: %v %+v", err, peers)
	}
	var pm portmapView
	if err := json.Unmarshal([]byte(answerFor(tn, admin, "portmap-v1")), &pm); err != nil || pm.Allowed || pm.Mapper != "off" {
		t.Fatalf("portmap-v1: %v %+v", err, pm)
	}
	raw := answerFor(tn, admin, "machine-v1")
	var view machineView
	if err := json.Unmarshal([]byte(raw), &view); err != nil {
		t.Fatalf("machine-v1: %v %s", err, raw)
	}
	if view.Mode != "machine" || view.MachineID != machineA || view.PeerName != "onv-m-"+machineA || view.Joined == nil ||
		view.JoinPending || view.State != stateRunning || view.Address != "100.64.0.1" {
		t.Fatalf("machine-v1: %+v", view)
	}
	if strings.Contains(raw, keyOne) || strings.Contains(raw, "fixture-machine-identity") {
		t.Fatalf("machine-v1 says a secret: %s", raw)
	}

	// An administrator's stop holds; the loop does not undo it.
	if got := answerFor(tn, admin, "stop"); got != "ok" {
		t.Fatalf("stop: %q", got)
	}
	if got := tn.machineStep(time.Now().Add(time.Hour)); got != "" {
		t.Fatalf("the loop undid a hold: %q", got)
	}
	if got := answerFor(tn, admin, "resume"); got != "ok" {
		t.Fatalf("resume: %q", got)
	}
	finish(t, tn)
	if state, why := stateOf(tn); state != stateRunning {
		t.Fatalf("state %d: %s", state, why)
	}
}

func TestADeviceKnowsNoMachineRequest(t *testing.T) {
	tn := testTunnel(t)
	if got := answer(tn, "machine-v1"); !strings.HasPrefix(got, "err unknown request") {
		t.Fatalf("machine-v1 on a device: %q", got)
	}
	if tn.directory() != tn.dir {
		t.Fatalf("a device's identity moved: %s", tn.directory())
	}
}

func TestMachineBackoffDoublesToItsCeiling(t *testing.T) {
	want := []time.Duration{2, 2, 4, 8, 16, 32, 60, 60}
	for i, w := range want {
		if got := machineBackoff(i); got != w*time.Second {
			t.Errorf("backoff(%d) = %v, want %v", i, got, w*time.Second)
		}
	}
}
