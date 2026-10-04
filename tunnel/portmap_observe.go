package main

// NetBird's port mapper, observed: what it asked the router for, and what
// the router answered.
//
// **The mapper is NetBird's, not ours.** NetBird 0.78.1's engine starts it
// on every connection (`client/internal/engine.go:661`): it asks the router
// in front of this machine for a UDP mapping of the WireGuard port over PCP,
// NAT-PMP or UPnP-IGD, with a two-hour lease (`portforward/manager.go:18`),
// renews it at half the lease (`manager.go:222`), offers the mapped address
// to the other peer as an extra ICE candidate (`peer/worker_ice.go:429`),
// and deletes it when the engine stops (`manager.go:319`). Whether it runs at
// all is the buyer's, and portmap.go holds that switch.
//
// **Why its log rather than an API (the exception the rules allow).** The
// manager's own state, `Manager.GetMapping`, lives in `client/internal`,
// which Go forbids anything outside NetBird's module to import, and
// `client/embed` exposes neither the manager nor a status field for it. Its
// log lines are the only interface it has, so they are read here, once each,
// as they are written: a logrus hook, not a file parser. Every sentence
// matched below is asserted to exist, verbatim, in the pinned source by
// TestTheMapperSentencesAreNetBirdsOwn, so an upgrade that rewords one fails
// a test instead of silently reporting nothing.
//
// What the log cannot say: a renewal that worked is logged at debug
// (`manager.go:315`), and this daemon runs NetBird at info. So `renew_due`
// is computed from the lease, and only a renewal that *failed* is observed.

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/sirupsen/logrus"
)

// NetBird's lease (`manager.go:18`, `defaultMappingTTL = 2 * time.Hour`),
// which no log line carries. Pinned by the source test with the sentences.
const netbirdMappingTTL = 2 * time.Hour

// What the mapper did, as NetBird said it.
const (
	portmapUnknown      = "unknown"       // nothing said since the tunnel started
	portmapDisabled     = "disabled"      // the switch held: NetBird said it is off
	portmapDiscovering  = "discovering"   // a gateway answered; no mapping yet
	portmapMapped       = "mapped"        // the router holds a mapping
	portmapNone         = "none"          // no router answered, or it refused
	portmapDeleted      = "deleted"       // removed from the router
	portmapDeleteFailed = "delete-failed" // still on the router; retried at the next start
	portmapStopped      = "stopped"       // the tunnel stopped with nothing mapped
)

// The mapping's state, as `portmap-v1` reports it.
type portmapState struct {
	State        string     `json:"state"`
	Gateway      string     `json:"gateway,omitempty"` // the protocol, in go-nat's words: "NAT-PMP", "UPnP (IGD2-IP1)", "PCP", …
	Protocol     string     `json:"protocol,omitempty"`
	InternalPort int        `json:"internal_port,omitempty"`
	ExternalIP   string     `json:"external_ip,omitempty"`
	ExternalPort int        `json:"external_port,omitempty"`
	LeaseSeconds int64      `json:"lease_seconds,omitempty"`
	Permanent    bool       `json:"permanent,omitempty"`
	Since        time.Time  `json:"since"`
	RenewDue     *time.Time `json:"renew_due,omitempty"`
	Reason       string     `json:"reason,omitempty"`
	RenewError   string     `json:"renew_error,omitempty"`
}

// What is on a router because of this machine, kept on disk until it is
// known to be gone: a crash, a lost power cord or a delete NetBird only
// pretended to make (NAT-PMP, portmap_release.go) all leave one behind, and
// the next start removes it. One mapping, because NetBird makes one.
type leaseRecord struct {
	Gateway      string    `json:"gateway"`
	Protocol     string    `json:"protocol"`
	InternalPort int       `json:"internal_port"`
	ExternalPort int       `json:"external_port"`
	ExternalIP   string    `json:"external_ip,omitempty"`
	Permanent    bool      `json:"permanent,omitempty"`
	Created      time.Time `json:"created"`
}

func (r leaseRecord) expires() time.Time {
	if r.Permanent {
		return time.Time{}
	}
	return r.Created.Add(netbirdMappingTTL)
}

// The sentences, from netbird v0.78.1 client/internal/portforward/manager.go
// and env.go. Each is anchored: a line that merely contains one is not it.
var (
	reMapperDisabled   = regexp.MustCompile(`^NAT port mapper disabled via (\S+)$`)
	reZeroPort         = regexp.MustCompile(`^invalid WireGuard port 0; NAT mapping disabled$`)
	reDiscovered       = regexp.MustCompile(`^discovered NAT gateway: (.+)$`)
	reSetupFailed      = regexp.MustCompile(`^port forwarding setup: (.+)$`)
	rePermanentOnly    = regexp.MustCompile(`^gateway only supports permanent leases, retrying with indefinite duration$`)
	reCreated          = regexp.MustCompile(`^created port mapping: (\d+) -> (\d+) via (.+) \(external IP: (.*)\)$`)
	reRenewFailed      = regexp.MustCompile(`^failed to renew port mapping: (.+)$`)
	rePortChanged      = regexp.MustCompile(`^external port changed on renewal: (\d+) -> (\d+) \(candidate may be stale\)$`)
	reRecreateFailed   = regexp.MustCompile(`^failed to recreate port mapping after server restart: (.+)$`)
	reDeleted          = regexp.MustCompile(`^deleted port mapping for port (\d+)$`)
	reDeleteFailed     = regexp.MustCompile(`^delete port mapping on stop: (.+)$`)
	portmapFirstWords  = []string{"NAT port mapper", "invalid WireGuard port", "discovered NAT gateway", "port forwarding setup", "gateway only supports", "created port mapping", "failed to renew port mapping", "external port changed", "failed to recreate port mapping", "deleted port mapping", "delete port mapping on stop"}
	errNoRecordToWrite = errors.New("no mapping to record")
)

type portmapObserver struct {
	mu         sync.Mutex
	state      portmapState
	permanent  bool // the next mapping is a permanent one (UPnP 725)
	recordPath string
	now        func() time.Time
}

func newPortmapObserver(recordPath string) *portmapObserver {
	o := &portmapObserver{recordPath: recordPath, now: time.Now}
	o.state = portmapState{State: portmapUnknown, Since: o.now().UTC()}
	return o
}

// Levels: the mapper's sentences are at info and warn, and its failures at
// error. Debug is not on in this daemon (main.go), so asking for it buys
// nothing.
func (o *portmapObserver) Levels() []logrus.Level {
	return []logrus.Level{logrus.ErrorLevel, logrus.WarnLevel, logrus.InfoLevel}
}

func (o *portmapObserver) Fire(entry *logrus.Entry) error {
	o.observe(entry.Message)
	return nil
}

// A new tunnel generation: what the last one said is history.
func (o *portmapObserver) begin() {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.permanent = false
	o.state = portmapState{State: portmapUnknown, Since: o.now().UTC()}
}

// The tunnel stopped. A mapping NetBird deleted said so already; one it
// could not delete, or one the release below has yet to remove, keeps its
// state.
func (o *portmapObserver) stopped() {
	o.mu.Lock()
	defer o.mu.Unlock()
	switch o.state.State {
	case portmapMapped, portmapDeleteFailed, portmapDeleted:
		return
	}
	o.state = portmapState{State: portmapStopped, Since: o.now().UTC()}
}

func (o *portmapObserver) snapshot() portmapState {
	o.mu.Lock()
	defer o.mu.Unlock()
	s := o.state
	if s.State == portmapMapped && !s.Permanent && s.LeaseSeconds > 0 {
		// NetBird renews every half lease from creation (`manager.go:222`).
		half := time.Duration(s.LeaseSeconds) * time.Second / 2
		due := s.Since.Add(half)
		for now := o.now(); !due.After(now); due = due.Add(half) {
		}
		s.RenewDue = &due
	}
	return s
}

func (o *portmapObserver) observe(message string) {
	message = strings.TrimSpace(message)
	relevant := false
	for _, prefix := range portmapFirstWords {
		if strings.HasPrefix(message, prefix) {
			relevant = true
			break
		}
	}
	if !relevant {
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	now := o.now().UTC()
	var record *leaseRecord
	dropRecord := false
	switch {
	case reMapperDisabled.MatchString(message):
		o.state = portmapState{State: portmapDisabled, Since: now}
	case reZeroPort.MatchString(message):
		o.state = portmapState{State: portmapNone, Since: now, Reason: "the WireGuard port is 0"}
	case reDiscovered.MatchString(message):
		m := reDiscovered.FindStringSubmatch(message)
		o.state = portmapState{State: portmapDiscovering, Since: now, Gateway: m[1]}
	case reSetupFailed.MatchString(message):
		m := reSetupFailed.FindStringSubmatch(message)
		o.state = portmapState{State: portmapNone, Since: now, Gateway: o.state.Gateway, Reason: m[1]}
	case rePermanentOnly.MatchString(message):
		o.permanent = true
	case reCreated.MatchString(message):
		m := reCreated.FindStringSubmatch(message)
		internal, _ := strconv.Atoi(m[1])
		external, _ := strconv.Atoi(m[2])
		ip := m[4]
		if ip == "<nil>" {
			ip = ""
		}
		o.state = portmapState{
			State: portmapMapped, Since: now, Gateway: m[3], Protocol: "udp",
			InternalPort: internal, ExternalPort: external, ExternalIP: ip,
			Permanent: o.permanent,
		}
		if !o.permanent {
			o.state.LeaseSeconds = int64(netbirdMappingTTL / time.Second)
		}
		record = &leaseRecord{
			Gateway: m[3], Protocol: "udp", InternalPort: internal, ExternalPort: external,
			ExternalIP: ip, Permanent: o.permanent, Created: now,
		}
	case reRenewFailed.MatchString(message):
		o.state.RenewError = reRenewFailed.FindStringSubmatch(message)[1]
	case reRecreateFailed.MatchString(message):
		o.state.RenewError = reRecreateFailed.FindStringSubmatch(message)[1]
	case rePortChanged.MatchString(message):
		m := rePortChanged.FindStringSubmatch(message)
		external, _ := strconv.Atoi(m[2])
		o.state.ExternalPort = external
		if saved, err := readLeaseRecord(o.recordPath); err == nil && saved != nil {
			saved.ExternalPort = external
			record = saved
		}
	case reDeleted.MatchString(message):
		port, _ := strconv.Atoi(reDeleted.FindStringSubmatch(message)[1])
		// **NAT-PMP's delete is not one.** go-nat's natpmpNAT.DeletePortMapping
		// forgets the port and sends nothing (go-nat natpmp.go:125), and
		// NetBird logs this sentence regardless, so the router keeps the
		// mapping for the rest of its lease. The record stays, and
		// portmap_release.go sends the delete the protocol defines.
		if strings.Contains(o.state.Gateway, "NAT-PMP") {
			o.state.Reason = "NetBird forgot the mapping; the router has not been told yet"
			break
		}
		o.state = portmapState{State: portmapDeleted, Since: now, Gateway: o.state.Gateway, InternalPort: port}
		dropRecord = true
	case reDeleteFailed.MatchString(message):
		o.state.State = portmapDeleteFailed
		o.state.Reason = reDeleteFailed.FindStringSubmatch(message)[1]
	}
	// The record is written beside the state, under the same lock, so the
	// file never says less than the state does.
	if record != nil {
		if err := writeLeaseRecord(o.recordPath, *record); err != nil {
			fmt.Fprintf(os.Stderr, "onv-tunnel: could not record the port mapping: %v\n", err)
		}
	}
	if dropRecord {
		_ = removeLeaseRecord(o.recordPath)
	}
}

// After a release (portmap_release.go) removed what was on the router.
func (o *portmapObserver) released(rec leaseRecord) {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.state = portmapState{State: portmapDeleted, Since: o.now().UTC(), Gateway: rec.Gateway, Protocol: rec.Protocol, InternalPort: rec.InternalPort}
}

func (o *portmapObserver) releaseFailed(rec leaseRecord, why string) {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.state = portmapState{State: portmapDeleteFailed, Since: o.now().UTC(), Gateway: rec.Gateway, Protocol: rec.Protocol, InternalPort: rec.InternalPort, ExternalPort: rec.ExternalPort, Reason: why}
}

func readLeaseRecord(path string) (*leaseRecord, error) {
	if path == "" {
		return nil, nil
	}
	raw, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var rec leaseRecord
	if err := json.Unmarshal(raw, &rec); err != nil || rec.InternalPort <= 0 || rec.Protocol != "udp" {
		return nil, fmt.Errorf("the port mapping record is unreadable")
	}
	return &rec, nil
}

func writeLeaseRecord(path string, rec leaseRecord) error {
	if path == "" {
		return errNoRecordToWrite
	}
	raw, err := json.Marshal(rec)
	if err != nil {
		return err
	}
	return writeFileAtomic(path, raw)
}

func removeLeaseRecord(path string) error {
	if path == "" {
		return nil
	}
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

// Written beside itself and renamed, so a reader sees the old file or the
// new one and never half of either.
func writeFileAtomic(path string, raw []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, raw, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}
