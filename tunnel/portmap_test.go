package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"

	netbird "github.com/netbirdio/netbird/client/embed"
	"github.com/sirupsen/logrus"
)

// What NetBird 0.78.1 writes, verbatim in shape (manager.go:163, 209, 334).
const (
	sayDiscovered = "discovered NAT gateway: %s"
	sayCreated    = "created port mapping: 51820 -> %d via %s (external IP: 203.0.113.7)"
	sayDeleted    = "deleted port mapping for port 51820"
)

// A tunnel whose fake engine says what NetBird's mapper would, if the switch
// it was started with lets it run, and records that switch.
type mapperFixture struct {
	tn       *tunnel
	mu       sync.Mutex
	switches []string // NB_DISABLE_NAT_MAPPER as each start found it
	stops    int
	gateway  string
	external int
}

func newMapperFixture(t *testing.T, gateway string) *mapperFixture {
	t.Helper()
	f := &mapperFixture{tn: &tunnel{dir: t.TempDir()}, gateway: gateway, external: 40123}
	hooks := logrus.StandardLogger().ReplaceHooks(make(logrus.LevelHooks))
	output := logrus.StandardLogger().Out
	logrus.SetOutput(io.Discard)
	logrus.AddHook(f.tn.portmapObs())
	t.Cleanup(func() {
		logrus.StandardLogger().ReplaceHooks(hooks)
		logrus.SetOutput(output)
	})
	// Whatever launched the test decides nothing.
	_ = os.Unsetenv(natMapperEnv)
	f.tn.factory = func(opts netbird.Options) (tunnelClient, error) {
		identity := opts.PrivateKey
		if opts.SetupKey != "" {
			identity = "fixture-private-identity-" + opts.SetupKey
			if err := writeIdentity(opts.ConfigPath, identity); err != nil {
				return nil, err
			}
		}
		mapped := false
		return &fakeClient{
			identity: identity,
			start: func(context.Context) error {
				// What manager.go:72 reads, at the moment it reads it.
				value := os.Getenv(natMapperEnv)
				f.mu.Lock()
				f.switches = append(f.switches, value)
				f.mu.Unlock()
				if value == "true" {
					logrus.Infof("NAT port mapper disabled via %s", natMapperEnv)
					return nil
				}
				logrus.Infof(sayDiscovered, f.gateway)
				logrus.Infof(sayCreated, f.external, f.gateway)
				mapped = true
				return nil
			},
			stop: func(context.Context) error {
				f.mu.Lock()
				f.stops++
				f.mu.Unlock()
				if mapped {
					// manager.go:334, logged for NAT-PMP too though nothing was sent.
					logrus.Info(sayDeleted)
					mapped = false
				}
				return nil
			},
		}, nil
	}
	t.Cleanup(func() { _ = f.tn.stop() })
	return f
}

func (f *mapperFixture) lastSwitch(t *testing.T) string {
	t.Helper()
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.switches) == 0 {
		t.Fatal("no engine started")
	}
	return f.switches[len(f.switches)-1]
}

func viewPortmap(t *testing.T, tn *tunnel) portmapView {
	t.Helper()
	raw := answer(tn, "portmap-v1")
	var view portmapView
	if err := json.Unmarshal([]byte(raw), &view); err != nil {
		t.Fatalf("portmap-v1 answered %q: %v", raw, err)
	}
	return view
}

func allow(t *testing.T, tn *tunnel, on bool) {
	t.Helper()
	word := "off"
	if on {
		word = "on"
	}
	if got := answer(tn, "portmap-v1 "+word); got != "ok" {
		t.Fatalf("portmap-v1 %s: %s", word, got)
	}
	finish(t, tn)
}

func noReleases(t *testing.T) {
	t.Helper()
	nat, upnp := releaseNATPMP, releaseUPnP
	releaseNATPMP = func(context.Context, leaseRecord) error { t.Error("unexpected NAT-PMP release"); return nil }
	releaseUPnP = func(context.Context, leaseRecord) error { t.Error("unexpected UPnP release"); return nil }
	t.Cleanup(func() { releaseNATPMP, releaseUPnP = nat, upnp })
}

// **On unless the owner said no** (5 October 2026) — the old default (no
// file means off) fails here.
func TestTheRouterIsAskedUnlessTheOwnerSaidNo(t *testing.T) {
	noReleases(t)
	f := newMapperFixture(t, "UPNP (IG2-IP1)")
	enroll(t, f.tn)
	if got := f.lastSwitch(t); got != "false" {
		t.Fatalf("a new enrolment started NetBird with %s=%q; want false (mapper on)", natMapperEnv, got)
	}
	view := viewPortmap(t, f.tn)
	if !view.Allowed || view.Mapper != "on" {
		t.Fatalf("default view %+v; want allowed, mapper on", view)
	}

	// And a plain resume, the daemon's own start at boot, is on too.
	if err := f.tn.stop(); err != nil {
		t.Fatal(err)
	}
	if err := f.tn.start("", ""); err != nil {
		t.Fatal(err)
	}
	finish(t, f.tn)
	if got := f.lastSwitch(t); got != "false" {
		t.Fatalf("a resume with no recorded choice started NetBird with %q", got)
	}
}

func TestTheOwnersNoIsKeptAndANewIdentityStartsOn(t *testing.T) {
	noReleases(t)
	f := newMapperFixture(t, "UPNP (IG2-IP1)")
	enroll(t, f.tn)
	allow(t, f.tn, false)
	if got := f.lastSwitch(t); got != "true" {
		t.Fatalf("refused, but NetBird started with %s=%q", natMapperEnv, got)
	}
	view := viewPortmap(t, f.tn)
	if view.Allowed || view.Mapper != "off" || view.State != portmapDisabled {
		t.Fatalf("view %+v; want not allowed, off, disabled", view)
	}
	if _, err := os.Stat(f.tn.leaseRecordPath()); err == nil {
		if rec, _ := readLeaseRecord(f.tn.leaseRecordPath()); rec != nil {
			t.Fatalf("a lease is still recorded after the owner said no: %+v", rec)
		}
	}
	// A resume keeps the owner's no.
	if err := f.tn.stop(); err != nil {
		t.Fatal(err)
	}
	if err := f.tn.start("", ""); err != nil {
		t.Fatal(err)
	}
	finish(t, f.tn)
	if got := f.lastSwitch(t); got != "true" {
		t.Fatalf("a resume after the owner said no started NetBird with %q", got)
	}

	// A new key is a new identity: the default again, whatever the old one said.
	request := enrolRequest{scope(), "https://netbird.lab.omnuv.com", "another-key", viewOf(t, f.tn).Revision}
	if got := answer(f.tn, "enrol-v1 "+payload(request)); got != "ok" {
		t.Fatal(got)
	}
	finish(t, f.tn)
	if got := f.lastSwitch(t); got != "false" {
		t.Fatalf("a re-enrolled identity started with %q; want the default, on", got)
	}
	if view := viewPortmap(t, f.tn); !view.Allowed {
		t.Fatalf("the new identity inherited the old one's no: %+v", view)
	}
}

// **Deleted when the tunnel stops** — the mutation that drops client.Stop
// from stopLocked, or the settle after it, fails here.
func TestStoppingTheTunnelDeletesTheMapping(t *testing.T) {
	noReleases(t)
	f := newMapperFixture(t, "UPNP (IG2-IP1)")
	view := enroll(t, f.tn)
	allow(t, f.tn, true)
	stops := f.stops
	request := stopRequest{Membership: *view.Membership, ExpectedRevision: viewOf(t, f.tn).Revision}
	if got := answer(f.tn, "stop-v1 "+payload(request)); got != "ok" {
		t.Fatal(got)
	}
	if f.stops != stops+1 {
		t.Fatalf("the engine was stopped %d times; want once", f.stops-stops)
	}
	got := viewPortmap(t, f.tn)
	if got.State != portmapDeleted || got.Mapper != "" {
		t.Fatalf("after the membership ended: %+v", got)
	}
	if _, err := os.Stat(f.tn.leaseRecordPath()); !os.IsNotExist(err) {
		t.Fatalf("the lease record outlived its mapping: %v", err)
	}
}

func TestTurningItOffRestartsTheTunnelWithoutTheMapper(t *testing.T) {
	noReleases(t)
	f := newMapperFixture(t, "UPNP (IG2-IP1)")
	enroll(t, f.tn)
	allow(t, f.tn, true)
	stops := f.stops
	allow(t, f.tn, false)
	if f.stops != stops+1 {
		t.Fatal("withdrawing consent did not stop the engine holding the mapping")
	}
	if got := f.lastSwitch(t); got != "true" {
		t.Fatalf("restarted with %q after the owner said no", got)
	}
	view := viewPortmap(t, f.tn)
	if view.Allowed || view.Mapper != "off" || view.State != portmapDisabled {
		t.Fatalf("after no: %+v", view)
	}
	// The same answer twice restarts nothing.
	stops = f.stops
	allow(t, f.tn, false)
	if f.stops != stops {
		t.Fatal("an unchanged choice restarted the tunnel")
	}
}

// NetBird says "deleted" for NAT-PMP and sends nothing (go-nat natpmp.go:125).
func TestANATPMPMappingIsDeletedByUsBecauseNetBirdDoesNot(t *testing.T) {
	var sent []leaseRecord
	fail := true
	nat, upnp := releaseNATPMP, releaseUPnP
	releaseNATPMP = func(_ context.Context, rec leaseRecord) error {
		sent = append(sent, rec)
		if fail {
			return errors.New("no answer from 10.0.0.1:5351")
		}
		return nil
	}
	releaseUPnP = func(context.Context, leaseRecord) error { t.Error("unexpected UPnP release"); return nil }
	t.Cleanup(func() { releaseNATPMP, releaseUPnP = nat, upnp })

	f := newMapperFixture(t, "NAT-PMP")
	enroll(t, f.tn)
	allow(t, f.tn, true)
	if err := f.tn.stop(); err != nil {
		t.Fatal(err)
	}
	if len(sent) != 1 || sent[0].InternalPort != 51820 || sent[0].Protocol != "udp" {
		t.Fatalf("NAT-PMP deletes sent: %+v", sent)
	}
	view := viewPortmap(t, f.tn)
	if view.State != portmapDeleteFailed || !strings.Contains(view.Leftover, "still on the router") {
		t.Fatalf("a failed delete reported as %+v", view)
	}
	if rec, _ := readLeaseRecord(f.tn.leaseRecordPath()); rec == nil {
		t.Fatal("a mapping still on the router lost its record")
	}
	// The next start tries again, before NetBird runs.
	fail = false
	if err := f.tn.start("", ""); err != nil {
		t.Fatal(err)
	}
	finish(t, f.tn)
	if len(sent) != 2 {
		t.Fatalf("the next start did not retry the delete: %d sent", len(sent))
	}
	if rec, _ := readLeaseRecord(f.tn.leaseRecordPath()); rec != nil {
		// The new engine mapped again (consent stands), so a record is
		// expected only if it was re-created.
		if rec.Created.IsZero() {
			t.Fatal("stale record survived")
		}
	}
}

// A crash leaves a UPnP mapping whose external port only the record knows.
func TestALeftoverFromACrashIsRemovedAtTheNextStart(t *testing.T) {
	var removed []leaseRecord
	nat, upnp := releaseNATPMP, releaseUPnP
	releaseNATPMP = func(context.Context, leaseRecord) error { t.Error("unexpected NAT-PMP release"); return nil }
	releaseUPnP = func(_ context.Context, rec leaseRecord) error { removed = append(removed, rec); return nil }
	t.Cleanup(func() { releaseNATPMP, releaseUPnP = nat, upnp })

	f := newMapperFixture(t, "UPNP (IG1-IP1)")
	enroll(t, f.tn)
	allow(t, f.tn, false) // the mapper off, so only the record knows the port
	crash := leaseRecord{Gateway: "UPNP (IG1-IP1)", Protocol: "udp", InternalPort: 51820, ExternalPort: 55555, Permanent: true, Created: time.Now().UTC()}
	if err := writeLeaseRecord(f.tn.leaseRecordPath(), crash); err != nil {
		t.Fatal(err)
	}
	if err := f.tn.start("", ""); err != nil { // running already: nothing happens
		t.Fatal(err)
	}
	if err := f.tn.stop(); err != nil {
		t.Fatal(err)
	}
	if len(removed) != 1 || removed[0].ExternalPort != 55555 || !removed[0].Permanent {
		t.Fatalf("removed %+v", removed)
	}
	if view := viewPortmap(t, f.tn); view.State != portmapDeleted || !strings.Contains(view.Leftover, "removed from the router") {
		t.Fatalf("after the leftover went: %+v", view)
	}
}

func TestAPCPLeftoverIsLeftToItsLeaseAndSaysWhen(t *testing.T) {
	noReleases(t)
	dir := t.TempDir()
	path := filepath.Join(dir, "portmap-lease.json")
	created := time.Date(2026, 10, 4, 12, 0, 0, 0, time.UTC)
	if err := writeLeaseRecord(path, leaseRecord{Gateway: "PCP", Protocol: "udp", InternalPort: 51820, ExternalPort: 51820, Created: created}); err != nil {
		t.Fatal(err)
	}
	said, gone := releaseLeftover(context.Background(), path, created.Add(time.Hour))
	if gone || !strings.Contains(said, "expires at 2026-10-04T14:00:00Z") {
		t.Fatalf("%q gone=%t", said, gone)
	}
	said, gone = releaseLeftover(context.Background(), path, created.Add(3*time.Hour))
	if !gone || !strings.Contains(said, "expired") {
		t.Fatalf("%q gone=%t", said, gone)
	}
	if rec, _ := readLeaseRecord(path); rec != nil {
		t.Fatal("an expired PCP record was kept")
	}
}

type fakeIGD struct {
	internalPort uint16
	client       string
	description  string
	lookupErr    error
	deleted      []uint16
}

func (g *fakeIGD) GetSpecificPortMappingEntryCtx(_ context.Context, _ string, port uint16, _ string) (uint16, string, bool, string, uint32, error) {
	return g.internalPort, g.client, true, g.description, 7200, g.lookupErr
}
func (g *fakeIGD) DeletePortMappingCtx(_ context.Context, _ string, port uint16, _ string) error {
	g.deleted = append(g.deleted, port)
	return nil
}

// Only this machine's NetBird mapping of this port is ever deleted.
func TestAUPnPLeftoverIsDeletedOnlyWhenItIsOurs(t *testing.T) {
	rec := leaseRecord{Gateway: "UPNP (IG2-IP1)", Protocol: "udp", InternalPort: 51820, ExternalPort: 40123}
	mine := map[string]bool{"192.168.1.20": true}
	cases := []struct {
		name   string
		igd    fakeIGD
		delete bool
	}{
		{"ours", fakeIGD{internalPort: 51820, client: "192.168.1.20", description: "NetBird"}, true},
		{"another machine", fakeIGD{internalPort: 51820, client: "192.168.1.21", description: "NetBird"}, false},
		{"another port", fakeIGD{internalPort: 51821, client: "192.168.1.20", description: "NetBird"}, false},
		{"another program", fakeIGD{internalPort: 51820, client: "192.168.1.20", description: "Steam"}, false},
	}
	for _, c := range cases {
		igd := c.igd
		err := deleteOwnedUPnP(context.Background(), []upnpConnection{&igd}, rec, mine)
		if c.delete && (err != nil || len(igd.deleted) != 1 || igd.deleted[0] != 40123) {
			t.Errorf("%s: err %v, deleted %v", c.name, err, igd.deleted)
		}
		if !c.delete && (err == nil || len(igd.deleted) != 0) {
			t.Errorf("%s: deleted %v (err %v); must be left alone", c.name, igd.deleted, err)
		}
	}
	gone := fakeIGD{lookupErr: errors.New("SOAP fault. Code:  | Explanation:  | Detail: <UPnPError><errorCode>714</errorCode><errorDescription>NoSuchEntryInArray</errorDescription></UPnPError>")}
	if err := deleteOwnedUPnP(context.Background(), []upnpConnection{&gone}, rec, mine); err != nil || len(gone.deleted) != 0 {
		t.Fatalf("an entry already gone: %v, %v", err, gone.deleted)
	}
	// 7140 is not 714.
	other := fakeIGD{lookupErr: errors.New("connect 10.0.0.1:7140: refused")}
	if err := deleteOwnedUPnP(context.Background(), []upnpConnection{&other}, rec, mine); err == nil {
		t.Fatal("a failed lookup read as an entry already gone")
	}
}

func TestTheObserverReadsEachSentenceAndNothingElse(t *testing.T) {
	path := filepath.Join(t.TempDir(), "lease.json")
	o := newPortmapObserver(path)
	o.observe("onv-tunnel: created port mapping: 1 -> 2 via X (external IP: 1.2.3.4)") // not anchored at the start
	if o.snapshot().State != portmapUnknown {
		t.Fatal("matched a sentence inside another line")
	}
	o.observe("discovered NAT gateway: UPNP (IG1-PPP1)")
	if s := o.snapshot(); s.State != portmapDiscovering || s.Gateway != "UPNP (IG1-PPP1)" {
		t.Fatalf("%+v", s)
	}
	o.observe("gateway only supports permanent leases, retrying with indefinite duration")
	o.observe("created port mapping: 51820 -> 61000 via UPNP (IG1-PPP1) (external IP: <nil>)")
	s := o.snapshot()
	if s.State != portmapMapped || !s.Permanent || s.LeaseSeconds != 0 || s.RenewDue != nil || s.ExternalIP != "" || s.ExternalPort != 61000 {
		t.Fatalf("a permanent mapping read as %+v", s)
	}
	o.observe("external port changed on renewal: 61000 -> 61001 (candidate may be stale)")
	if rec, _ := readLeaseRecord(path); rec == nil || rec.ExternalPort != 61001 || !rec.Permanent {
		t.Fatalf("record after the port moved: %+v", rec)
	}
	o.observe("failed to renew port mapping: add port mapping: timeout")
	if s := o.snapshot(); s.RenewError != "add port mapping: timeout" || s.State != portmapMapped {
		t.Fatalf("%+v", s)
	}
	o.observe("delete port mapping on stop: SOAP fault")
	if s := o.snapshot(); s.State != portmapDeleteFailed {
		t.Fatalf("%+v", s)
	}
	if rec, _ := readLeaseRecord(path); rec == nil {
		t.Fatal("a failed delete dropped the record")
	}
	o.begin()
	o.observe("port forwarding setup: discover gateway: no NAT found: context deadline exceeded")
	if s := o.snapshot(); s.State != portmapNone || !strings.Contains(s.Reason, "no NAT found") {
		t.Fatalf("no router read as %+v", s)
	}

	// The renewal NetBird makes every half lease, computed: a debug line
	// this daemon never sees (manager.go:315).
	base := time.Date(2026, 10, 4, 12, 0, 0, 0, time.UTC)
	o.now = func() time.Time { return base }
	o.observe("created port mapping: 51820 -> 40123 via NAT-PMP (external IP: 203.0.113.7)")
	o.now = func() time.Time { return base.Add(90 * time.Minute) }
	if s := o.snapshot(); s.RenewDue == nil || !s.RenewDue.Equal(base.Add(2*time.Hour)) {
		t.Fatalf("renew due %v", s.RenewDue)
	}
}

// **The sentences this file reads are NetBird's own**, in the version
// go.mod pins — and so is the bug portmap_release.go works around. A
// NetBird upgrade that rewords a line, moves the switch or fixes NAT-PMP's
// delete fails here, not silently on a buyer's machine.
func TestTheMapperSentencesAreNetBirdsOwn(t *testing.T) {
	dir := func(module string) string {
		out, err := exec.Command("go", "list", "-m", "-f", "{{.Dir}}", module).Output()
		if err != nil {
			t.Fatalf("go list %s: %v", module, err)
		}
		return strings.TrimSpace(string(out))
	}
	read := func(path string) string {
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		return string(raw)
	}
	nb := dir("github.com/netbirdio/netbird")
	manager := read(filepath.Join(nb, "client/internal/portforward/manager.go"))
	for _, said := range []string{
		`log.Infof("NAT port mapper disabled via %s", envDisableNATMapper)`,
		`log.Warnf("invalid WireGuard port 0; NAT mapping disabled")`,
		`log.Infof("discovered NAT gateway: %s", gateway.Type())`,
		`log.Infof("port forwarding setup: %v", err)`,
		`log.Infof("gateway only supports permanent leases, retrying with indefinite duration")`,
		`log.Infof("created port mapping: %d -> %d via %s (external IP: %s)",`,
		`log.Warnf("failed to renew port mapping: %v", err)`,
		`log.Warnf("external port changed on renewal: %d -> %d (candidate may be stale)"`,
		`log.Errorf("failed to recreate port mapping after server restart: %v", err)`,
		`log.Infof("deleted port mapping for port %d", mapping.InternalPort)`,
		`log.Warnf("delete port mapping on stop: %v", err)`,
		`mappingDescription  = "NetBird"`,
		`externalPort, err := gateway.AddPortMapping(ctx, "udp", int(m.wgPort), mappingDescription, ttl)`,
	} {
		if !strings.Contains(manager, said) {
			t.Errorf("manager.go no longer says %s", said)
		}
	}
	if !regexp.MustCompile(`defaultMappingTTL\s*=\s*2 \* time\.Hour`).MatchString(manager) {
		t.Error("NetBird's lease is no longer two hours; netbirdMappingTTL is wrong")
	}
	if !regexp.MustCompile(`(?s)func \(m \*Manager\) Start\(.*?if isDisabledByEnv\(\)`).MatchString(manager) {
		t.Error("Start no longer reads the switch")
	}
	if !strings.Contains(read(filepath.Join(nb, "client/internal/portforward/env.go")), `envDisableNATMapper      = "NB_DISABLE_NAT_MAPPER"`) {
		t.Error("the switch is no longer NB_DISABLE_NAT_MAPPER")
	}
	if !strings.Contains(read(filepath.Join(nb, "client/internal/engine.go")), `e.portForwardManager.Start(e.ctx, uint16(e.config.WgPort))`) {
		t.Error("the engine no longer starts the mapper")
	}
	gonat := read(filepath.Join(dir("github.com/netbirdio/go-nat"), "natpmp.go"))
	if !regexp.MustCompile(`(?s)func \(n \*natpmpNAT\) DeletePortMapping\([^)]*\) \(err error\) \{\s*delete\(n\.ports, internalPort\)\s*return nil\s*\}`).MatchString(gonat) {
		t.Error("go-nat's NAT-PMP delete changed: if it now sends one, natpmpDelete is redundant")
	}
}

func TestPortmapV1IsTheOwnersAndItsReadClaimsNothing(t *testing.T) {
	noReleases(t)
	f := newMapperFixture(t, "UPNP (IG2-IP1)")
	if got := answer(f.tn, "portmap-v1 on"); !strings.Contains(got, "join this device") {
		t.Fatalf("consent with no identity: %q", got)
	}
	for _, bad := range []string{"portmap-v1 yes", "portmap-v1 on now"} {
		if got := answer(f.tn, bad); !strings.HasPrefix(got, "err portmap-v1 takes") {
			t.Errorf("%s: %q", bad, got)
		}
	}
	owner, other := alice, bob
	if got := answerFor(f.tn, owner, "portmap-v1"); !strings.HasPrefix(got, "{") {
		t.Fatalf("owner's read: %q", got)
	}
	if _, err := os.Stat(f.tn.ownerPath()); !os.IsNotExist(err) {
		t.Fatal("a read claimed the tunnel")
	}
	enroll(t, f.tn)
	if got := answerFor(f.tn, owner, "portmap-v1 on"); got != "ok" {
		t.Fatalf("owner's choice: %q", got)
	}
	finish(t, f.tn)
	if got := answerFor(f.tn, other, "portmap-v1"); !strings.HasPrefix(got, "err") {
		t.Fatalf("another user read the router mapping: %q", got)
	}
	if got := answerFor(f.tn, other, "portmap-v1 off"); !strings.HasPrefix(got, "err") {
		t.Fatalf("another user changed the owner's choice: %q", got)
	}
}
