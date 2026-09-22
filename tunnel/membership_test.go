package main

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	netbird "github.com/netbirdio/netbird/client/embed"
)

type fakeClient struct {
	start    func(context.Context) error
	stop     func(context.Context) error
	identity string
	address  func() (string, string)
}

func (f *fakeClient) Start(ctx context.Context) error {
	if f.start != nil {
		return f.start(ctx)
	}
	return nil
}
func (f *fakeClient) Stop(ctx context.Context) error {
	if f.stop != nil {
		return f.stop(ctx)
	}
	return nil
}
func (f *fakeClient) Address() (string, string) {
	if f.address != nil {
		return f.address()
	}
	return "100.64.0.1", "fixture"
}
func (f *fakeClient) Identity() (string, error) { return f.identity, nil }

func testTunnel(t *testing.T) *tunnel {
	t.Helper()
	tn := &tunnel{dir: t.TempDir()}
	tn.factory = func(opts netbird.Options) (tunnelClient, error) {
		identity := opts.PrivateKey
		if opts.SetupKey != "" {
			identity = "fixture-private-identity-" + opts.SetupKey
			if err := writeIdentity(opts.ConfigPath, identity); err != nil {
				return nil, err
			}
		}
		return &fakeClient{identity: identity}, nil
	}
	t.Cleanup(func() { _ = tn.stop() })
	return tn
}

func writeIdentity(path, identity string) error {
	raw, _ := json.Marshal(map[string]string{"PrivateKey": identity})
	return os.WriteFile(path, raw, 0o600)
}

func scope() membership {
	return membership{"https://api.lab.omnuv.com", "00000000-0000-4000-8000-000000000001",
		"00000000-0000-4000-8000-000000000002", "00000000-0000-4000-8000-000000000003", "00000000-0000-4000-8000-000000000004"}
}
func payload(value any) string {
	raw, _ := json.Marshal(value)
	return base64.StdEncoding.EncodeToString(raw)
}
func viewOf(t *testing.T, tn *tunnel) membershipView {
	t.Helper()
	var view membershipView
	if err := json.Unmarshal([]byte(answer(tn, "membership-v1")), &view); err != nil {
		t.Fatal(err)
	}
	return view
}
func finish(t *testing.T, tn *tunnel) {
	t.Helper()
	tn.mu.Lock()
	done := tn.done
	tn.mu.Unlock()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("fixture start did not finish")
	}
}
func enroll(t *testing.T, tn *tunnel) membershipView {
	t.Helper()
	request := enrolRequest{scope(), "https://netbird.lab.omnuv.com", "fixture-key", viewOf(t, tn).Revision}
	if got := answer(tn, "enrol-v1 "+payload(request)); got != "ok" {
		t.Fatal(got)
	}
	finish(t, tn)
	view := viewOf(t, tn)
	if !view.Verified || view.Membership == nil || *view.Membership != scope() {
		t.Fatalf("unverified successful build: %+v", view)
	}
	return view
}

func TestMembershipEmptyAndLegacyAreExplicitlyDifferent(t *testing.T) {
	tn := testTunnel(t)
	empty := viewOf(t, tn)
	if empty.Version != 1 || empty.Revision != "empty" || empty.HasIdentity || empty.Membership != nil {
		t.Fatalf("empty: %+v", empty)
	}
	const secret = "never-return-this-private-key"
	if err := writeIdentity(filepath.Join(tn.dir, "config.json"), secret); err != nil {
		t.Fatal(err)
	}
	legacy := viewOf(t, tn)
	if !legacy.HasIdentity || legacy.Verified || legacy.Membership != nil || legacy.Revision == "empty" {
		t.Fatalf("legacy: %+v", legacy)
	}
	if strings.Contains(answer(tn, "membership-v1"), secret) {
		t.Fatal("identity leaked into public IPC")
	}
	before, _ := os.ReadFile(filepath.Join(tn.dir, "config.json"))
	if got := answer(tn, "resume-v1 "+payload(scope())); !strings.HasPrefix(got, "err membership-mismatch") {
		t.Fatal(got)
	}
	after, _ := os.ReadFile(filepath.Join(tn.dir, "config.json"))
	if string(before) != string(after) {
		t.Fatal("unverified resume changed the identity")
	}
}

func TestEnrollmentCASAndDurableIdentityBinding(t *testing.T) {
	tn := testTunnel(t)
	old := enroll(t, tn)
	request := enrolRequest{scope(), "https://netbird.lab.omnuv.com", "new-key", "empty"}
	if got := answer(tn, "enrol-v1 "+payload(request)); !strings.HasPrefix(got, "err membership-changed") {
		t.Fatal(got)
	}
	if viewOf(t, tn).Revision != old.Revision {
		t.Fatal("stale consent mutated membership")
	}
	restarted := &tunnel{dir: tn.dir}
	if got := viewOf(t, restarted); !got.Verified || got.Revision != old.Revision {
		t.Fatalf("restart lost binding: %+v", got)
	}
	if err := writeIdentity(filepath.Join(tn.dirFor(scope().CoreURL), "config.json"), "external-different-private-key"); err != nil {
		t.Fatal(err)
	}
	changed := viewOf(t, tn)
	if changed.Verified || changed.Membership != nil || changed.Revision == old.Revision {
		t.Fatal("stale metadata labeled a different identity")
	}
}

func TestResumeAndStopRequireExactMembership(t *testing.T) {
	tn := testTunnel(t)
	old := enroll(t, tn)
	wrong := scope()
	wrong.ProjectID = "00000000-0000-4000-8000-000000000099"
	for _, command := range []string{"resume-v1 " + payload(wrong), "stop-v1 " + payload(stopRequest{wrong, old.Revision})} {
		if got := answer(tn, command); !strings.HasPrefix(got, "err ") {
			t.Fatal(got)
		}
	}
	if state, _ := tn.snapshot(); state != stateRunning {
		t.Fatal("wrong scope stopped the active tunnel")
	}
	if got := answer(tn, "stop-v1 "+payload(stopRequest{scope(), old.Revision})); got != "ok" {
		t.Fatal(got)
	}
	if !viewOf(t, tn).Verified {
		t.Fatal("stop erased an identity before server revocation")
	}
	if got := answer(tn, "resume-v1 "+payload(scope())); got != "ok" {
		t.Fatal(got)
	}
	finish(t, tn)
	if state, _ := tn.snapshot(); state != stateRunning {
		t.Fatal("matching identity did not resume")
	}
}

func TestPendingEnrollmentIsJournaledAndCanceledStartCannotRevive(t *testing.T) {
	tn := testTunnel(t)
	started := make(chan struct{})
	var stops atomic.Int32
	tn.factory = func(opts netbird.Options) (tunnelClient, error) {
		if err := writeIdentity(opts.ConfigPath, "new-pending-identity"); err != nil {
			return nil, err
		}
		return &fakeClient{identity: "new-pending-identity", start: func(ctx context.Context) error { close(started); <-ctx.Done(); return nil },
			stop: func(context.Context) error { stops.Add(1); return nil }}, nil
	}
	if err := tn.enrolMembership(enrolRequest{scope(), "https://netbird.lab.omnuv.com", "key", "empty"}); err != nil {
		t.Fatal(err)
	}
	<-started
	pending := viewOf(t, tn)
	if !pending.Pending || pending.Verified || pending.Membership == nil || pending.Revision == "empty" {
		t.Fatalf("pending intent missing: %+v", pending)
	}
	if err := tn.resumeMembership(scope()); err == nil {
		t.Fatal("pending identity was treated as verified")
	}
	if err := tn.stopMembership(stopRequest{scope(), pending.Revision}); err != nil {
		t.Fatal(err)
	}
	if state, _ := tn.snapshot(); state != stateStopped || stops.Load() != 1 {
		t.Fatal("old completion revived a stopped tunnel")
	}
	if got := viewOf(t, tn); got.Verified {
		t.Fatal("canceled start published verified membership")
	}
}

func TestOverlappingEnrollmentCannotPublishAnOlderMembership(t *testing.T) {
	tn := testTunnel(t)
	var calls atomic.Int32
	tn.factory = func(opts netbird.Options) (tunnelClient, error) {
		if err := writeIdentity(opts.ConfigPath, opts.SetupKey); err != nil {
			return nil, err
		}
		if calls.Add(1) == 1 {
			return &fakeClient{identity: opts.SetupKey, start: func(ctx context.Context) error { <-ctx.Done(); return errors.New("old failure") }}, nil
		}
		return &fakeClient{identity: opts.SetupKey}, nil
	}
	if err := tn.enrolMembership(enrolRequest{scope(), "https://netbird.lab.omnuv.com", "first", "empty"}); err != nil {
		t.Fatal(err)
	}
	second := scope()
	second.DeviceID = "00000000-0000-4000-8000-000000000098"
	if err := tn.enrolMembership(enrolRequest{second, "https://netbird.lab.omnuv.com", "second", viewOf(t, tn).Revision}); err != nil {
		t.Fatal(err)
	}
	finish(t, tn)
	if got := viewOf(t, tn); !got.Verified || got.Membership == nil || *got.Membership != second {
		t.Fatalf("older callback overwrote replacement: %+v", got)
	}
}

func TestDeletionFailureAndCorruptMetadataFailClosed(t *testing.T) {
	tn := testTunnel(t)
	if err := writeIdentity(filepath.Join(tn.dir, "config.json"), "held-identity"); err != nil {
		t.Fatal(err)
	}
	// Nowhere to set the identity aside: the key is never tried, and the
	// identity stays where it was.
	if err := os.WriteFile(asideDir(tn.dir), []byte("not a directory"), 0o600); err != nil {
		t.Fatal(err)
	}
	var called atomic.Bool
	tn.factory = func(netbird.Options) (tunnelClient, error) { called.Store(true); return &fakeClient{}, nil }
	if err := tn.start("https://netbird.lab.omnuv.com", "key"); err == nil || called.Load() {
		t.Fatal("failed identity cleanup was ignored")
	}
	if id, _ := readIdentity(filepath.Join(tn.dir, "config.json")); id != "held-identity" {
		t.Fatalf("a refused attempt moved the identity: %q", id)
	}
	if err := os.WriteFile(filepath.Join(tn.dir, "membership.json"), []byte("corrupt"), 0o600); err != nil {
		t.Fatal(err)
	}
	if got := answer(tn, "membership-v1"); !strings.HasPrefix(got, "err ") {
		t.Fatal(got)
	}
}

// **H3: a key is tried before the identity it replaces is given up.** A junk,
// spent or hostile key used to delete the device's membership first, so it
// fell off every network whatever the key turned out to be.
func TestAFailedKeyKeepsTheIdentityItWouldHaveReplaced(t *testing.T) {
	for _, path := range []string{"legacy", "v1"} {
		t.Run(path, func(t *testing.T) {
			tn := testTunnel(t)
			old := enroll(t, tn)
			tn.factory = func(opts netbird.Options) (tunnelClient, error) {
				if err := writeIdentity(opts.ConfigPath, "rejected-identity"); err != nil {
					return nil, err
				}
				return &fakeClient{identity: "rejected-identity", start: func(context.Context) error { return errors.New("invalid setup-key") }}, nil
			}
			var err error
			if path == "legacy" {
				err = tn.start("https://netbird.lab.omnuv.com", "junk-key")
			} else {
				err = tn.enrolMembership(enrolRequest{scope(), "https://netbird.lab.omnuv.com", "junk-key", old.Revision})
			}
			if err != nil {
				t.Fatal(err)
			}
			finish(t, tn)
			if state, why := tn.snapshot(); state != stateFailed || !strings.Contains(why, "previous identity was kept") {
				t.Fatalf("the failure does not say the identity was kept: %d %q", state, why)
			}
			if now := viewOf(t, tn); now.Revision != old.Revision || !now.Verified {
				t.Fatalf("a rejected key cost the device its membership: %+v", now)
			}
			if _, err := os.Stat(asideDir(tn.directory())); !os.IsNotExist(err) {
				t.Fatal("the restored identity was left aside as well")
			}
			// And it still comes up without a key.
			tn.factory = testTunnel(t).factory
			if got := answer(tn, "resume-v1 "+payload(scope())); got != "ok" {
				t.Fatal(got)
			}
			finish(t, tn)
			if state, _ := tn.snapshot(); state != stateRunning {
				t.Fatalf("the kept identity does not resume: %d", state)
			}
		})
	}
}

// And a key that works replaces it, leaving nothing aside.
func TestAWorkingKeyReplacesTheIdentityForGood(t *testing.T) {
	tn := testTunnel(t)
	old := enroll(t, tn)
	if err := tn.enrolMembership(enrolRequest{scope(), "https://netbird.lab.omnuv.com", "second-key", old.Revision}); err != nil {
		t.Fatal(err)
	}
	finish(t, tn)
	if id, _ := readIdentity(filepath.Join(tn.directory(), "config.json")); id != "fixture-private-identity-second-key" {
		t.Fatalf("the new identity is not the one in use: %q", id)
	}
	if _, err := os.Stat(asideDir(tn.directory())); !os.IsNotExist(err) {
		t.Fatal("the replaced identity was kept after the new one worked")
	}
}

// An attempt cancelled before it resolved leaves the old identity aside; the
// next operation brings it back rather than resuming a half-made one.
func TestAnInterruptedKeyIsRolledBackOnTheNextOperation(t *testing.T) {
	tn := testTunnel(t)
	old := enroll(t, tn)
	started := make(chan struct{})
	tn.factory = func(opts netbird.Options) (tunnelClient, error) {
		if err := writeIdentity(opts.ConfigPath, "half-made-identity"); err != nil {
			return nil, err
		}
		return &fakeClient{identity: "half-made-identity", start: func(ctx context.Context) error { close(started); <-ctx.Done(); return nil }}, nil
	}
	if err := tn.enrolMembership(enrolRequest{scope(), "https://netbird.lab.omnuv.com", "slow-key", old.Revision}); err != nil {
		t.Fatal(err)
	}
	<-started
	if err := tn.stop(); err != nil {
		t.Fatal(err)
	}
	tn.factory = testTunnel(t).factory
	if got := answer(tn, "resume-v1 "+payload(scope())); got != "ok" {
		t.Fatal(got)
	}
	finish(t, tn)
	if now := viewOf(t, tn); now.Revision != old.Revision || !now.Verified {
		t.Fatalf("the interrupted attempt was not rolled back: %+v", now)
	}
}

func TestMalformedVersionedRequestsCannotMutate(t *testing.T) {
	tn := testTunnel(t)
	for _, command := range []string{"enrol-v1 nonsense", "resume-v1 " + payload(map[string]string{"unknown": "field"}), "stop-v1 " + payload(stopRequest{}), "membership-v1 extra", "enrol-v1 " + strings.Repeat("A", 17000)} {
		if got := answer(tn, command); !strings.HasPrefix(got, "err ") {
			t.Fatal(got)
		}
		if viewOf(t, tn).Revision != "empty" {
			t.Fatal("malformed request changed the identity")
		}
	}
}

func TestStartFailureCleansUpAndCannotClaimVerifiedMembership(t *testing.T) {
	tn := testTunnel(t)
	var stops atomic.Int32
	tn.factory = func(opts netbird.Options) (tunnelClient, error) {
		if err := writeIdentity(opts.ConfigPath, "failed-start-identity"); err != nil {
			return nil, err
		}
		return &fakeClient{identity: "failed-start-identity", start: func(context.Context) error { return errors.New("fixture startup failed") },
			stop: func(context.Context) error { stops.Add(1); return nil }}, nil
	}
	if err := tn.enrolMembership(enrolRequest{scope(), "https://netbird.lab.omnuv.com", "key", "empty"}); err != nil {
		t.Fatal(err)
	}
	finish(t, tn)
	if state, why := tn.snapshot(); state != stateFailed || !strings.Contains(why, "fixture startup failed") || stops.Load() != 1 {
		t.Fatalf("startup failure or cleanup disappeared: %d %q %d", state, why, stops.Load())
	}
	if got := viewOf(t, tn); got.Verified || !got.Pending {
		t.Fatal("failed start must retain only unverified recovery intent")
	}
}

func TestStopFailurePreservesIdentityAndPreventsReplacement(t *testing.T) {
	tn := testTunnel(t)
	old := enroll(t, tn)
	tn.mu.Lock()
	tn.client.(*fakeClient).stop = func(context.Context) error { return errors.New("fixture stop failed") }
	tn.mu.Unlock()
	if err := tn.enrolMembership(enrolRequest{scope(), "https://netbird.lab.omnuv.com", "replacement", old.Revision}); err == nil {
		t.Fatal("an unproven stop allowed replacing an active identity")
	}
	if now := viewOf(t, tn); now.Revision != old.Revision || !now.Verified {
		t.Fatal("failed stop damaged old identity")
	}
}

func TestMembershipStateIsOneCoherentSnapshotAcrossReplacement(t *testing.T) {
	tn := testTunnel(t)
	old := enroll(t, tn)
	if old.State != stateRunning || old.Address == "" {
		t.Fatalf("missing versioned state/address: %+v", old)
	}
	entered, release := make(chan struct{}), make(chan struct{})
	tn.mu.Lock()
	tn.client.(*fakeClient).address = func() (string, string) { close(entered); <-release; return "old-address", "old" }
	tn.mu.Unlock()
	result := make(chan string, 1)
	go func() { result <- answer(tn, "membership-v1") }()
	<-entered
	newScope := scope()
	newScope.ProjectID = "00000000-0000-4000-8000-000000000077"
	if err := tn.enrolMembership(enrolRequest{newScope, "https://netbird.lab.omnuv.com", "new-key", old.Revision}); err != nil {
		t.Fatal(err)
	}
	finish(t, tn)
	close(release)
	if got := <-result; !strings.HasPrefix(got, "err membership changed") {
		t.Fatalf("mixed old scope and new state: %s", got)
	}
	if view := viewOf(t, tn); view.State != stateRunning || *view.Membership != newScope {
		t.Fatalf("new coherent snapshot wrong: %+v", view)
	}
}

func TestRunningLibraryIdentityMustMatchPersistedIdentity(t *testing.T) {
	tn := testTunnel(t)
	var stopped atomic.Bool
	tn.factory = func(opts netbird.Options) (tunnelClient, error) {
		if err := writeIdentity(opts.ConfigPath, "different-disk-identity"); err != nil {
			return nil, err
		}
		return &fakeClient{identity: "actual-library-identity", stop: func(context.Context) error { stopped.Store(true); return nil }}, nil
	}
	if err := tn.enrolMembership(enrolRequest{scope(), "https://netbird.lab.omnuv.com", "key", "empty"}); err != nil {
		t.Fatal(err)
	}
	finish(t, tn)
	if state, why := tn.snapshot(); state != stateFailed || !strings.Contains(why, "stored identity differs") || !stopped.Load() {
		t.Fatalf("false binding was not stopped: %d %q", state, why)
	}
	if viewOf(t, tn).Verified {
		t.Fatal("disk key was falsely bound to the running library's scope")
	}
}

func otherScope() membership {
	m := scope()
	m.CoreURL = "https://api.test.omnuv.com"
	m.DeviceID = "00000000-0000-4000-8000-000000000005"
	return m
}

func enrollAt(t *testing.T, tn *tunnel, m membership, key string) {
	t.Helper()
	var view membershipView
	if err := json.Unmarshal([]byte(answer(tn, "membership-v1 "+m.CoreURL)), &view); err != nil {
		t.Fatal(err)
	}
	request := enrolRequest{m, "https://netbird.lab.omnuv.com", key, view.Revision}
	if got := answer(tn, "enrol-v1 "+payload(request)); got != "ok" {
		t.Fatal(got)
	}
	finish(t, tn)
}

func identityAt(t *testing.T, tn *tunnel, coreURL string) string {
	t.Helper()
	id, err := readIdentity(filepath.Join(tn.dirFor(coreURL), "config.json"))
	if err != nil {
		t.Fatal(err)
	}
	return id
}

// **The point of the change.** Two Cores, two identities; moving between them
// keeps both, and moving back resumes the saved one without a key.
func TestEachDeploymentKeepsItsOwnIdentity(t *testing.T) {
	tn := testTunnel(t)
	enrollAt(t, tn, scope(), "prod-key")
	prod := identityAt(t, tn, scope().CoreURL)
	enrollAt(t, tn, otherScope(), "test-key")
	if identityAt(t, tn, scope().CoreURL) != prod {
		t.Fatal("enrolling another deployment replaced this one's identity")
	}
	if identityAt(t, tn, otherScope().CoreURL) == "" || identityAt(t, tn, otherScope().CoreURL) == prod {
		t.Fatal("the second deployment has no identity of its own")
	}
	var calls atomic.Int32
	factory := tn.factory
	tn.factory = func(opts netbird.Options) (tunnelClient, error) {
		if opts.SetupKey != "" {
			calls.Add(1)
		}
		return factory(opts)
	}
	if got := answer(tn, "resume-v1 "+payload(scope())); got != "ok" {
		t.Fatal(got)
	}
	finish(t, tn)
	if state, _ := tn.snapshot(); state != stateRunning || calls.Load() != 0 {
		t.Fatalf("switching back did not resume the saved identity without a key: state %d, keys %d", state, calls.Load())
	}
	if active := viewOf(t, tn); active.Membership == nil || *active.Membership != scope() {
		t.Fatalf("the running deployment is not the one resumed: %+v", active)
	}
}

// Reading another deployment does not switch to it, and reports it stopped.
func TestReadingADeploymentDoesNotSwitchToIt(t *testing.T) {
	tn := testTunnel(t)
	enrollAt(t, tn, otherScope(), "test-key")
	enrollAt(t, tn, scope(), "prod-key")
	var other membershipView
	if err := json.Unmarshal([]byte(answer(tn, "membership-v1 "+otherScope().CoreURL)), &other); err != nil {
		t.Fatal(err)
	}
	if !other.Verified || other.Membership == nil || *other.Membership != otherScope() || other.State != stateStopped {
		t.Fatalf("the other deployment read wrong: %+v", other)
	}
	if active := viewOf(t, tn); active.Membership == nil || *active.Membership != scope() || active.State != stateRunning {
		t.Fatalf("reading switched the running deployment: %+v", active)
	}
	if got := answer(tn, "membership-v1 not-a-url"); !strings.HasPrefix(got, "err ") {
		t.Fatal(got)
	}
}

// An identity from before this version moves into the Core its record names,
// once; one with no record stays in the base, never guessed into a deployment.
func TestAnEarlierIdentityMovesOnlyWhereItsRecordSays(t *testing.T) {
	base := t.TempDir()
	first := &tunnel{dir: base}
	first.factory = testTunnel(t).factory
	enrollAt(t, first, scope(), "old-key")
	saved := identityAt(t, first, scope().CoreURL)
	_ = first.stop()
	// Put it back where an earlier version kept it: the base, with no active file.
	for _, name := range []string{"config.json", "state.json", "membership.json"} {
		from := filepath.Join(first.dirFor(scope().CoreURL), name)
		if _, err := os.Stat(from); err == nil {
			if err := os.Rename(from, filepath.Join(base, name)); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := os.Remove(filepath.Join(base, "active")); err != nil {
		t.Fatal(err)
	}
	upgraded := &tunnel{dir: base}
	view := viewOf(t, upgraded)
	if !view.Verified || view.Membership == nil || *view.Membership != scope() {
		t.Fatalf("the earlier identity was not found in its deployment: %+v", view)
	}
	if identityAt(t, upgraded, scope().CoreURL) != saved {
		t.Fatal("the identity changed while it was moved")
	}
	if _, err := os.Stat(filepath.Join(base, "config.json")); !os.IsNotExist(err) {
		t.Fatal("a copy stayed in the base")
	}

	legacyBase := t.TempDir()
	if err := writeIdentity(filepath.Join(legacyBase, "config.json"), "legacy-with-no-record"); err != nil {
		t.Fatal(err)
	}
	legacy := &tunnel{dir: legacyBase}
	if view := viewOf(t, legacy); !view.HasIdentity || view.Verified || view.Membership != nil {
		t.Fatalf("an unnamed identity was labelled: %+v", view)
	}
	if _, err := os.Stat(filepath.Join(legacyBase, "deployments")); !os.IsNotExist(err) {
		t.Fatal("an unnamed identity was moved into a deployment")
	}
}
