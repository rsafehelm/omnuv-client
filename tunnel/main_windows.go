package main

import (
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/Microsoft/go-winio"
	"golang.org/x/sys/windows/svc"
)

// Everything the marketplace creates carries `onv`, including this.
const serviceName = "OnvTunnel"

func socketName() string { return `\\.\pipe\onv-tunnel` }

// Machine-wide, because the tunnel is. The service runs as LocalSystem, whose
// %APPDATA% is inside a system profile nobody can find, and the identity here
// belongs to the machine rather than to whoever happens to be logged in.
func configDir() string {
	base := os.Getenv("ProgramData")
	if base == "" {
		base = `C:\ProgramData`
	}
	return filepath.Join(base, "onv", "tunnel")
}

// **Who may speak to it**, in SDDL: Administrators get everything,
// authenticated users get read and write. The desktop client runs as the
// person who is logged in, and it has to be able to ask for a join — see the
// note on `serve` for what that means and does not mean.
const pipeSecurity = "D:P(A;;GA;;;BA)(A;;GA;;;SY)(A;;GRGW;;;AU)"

func listen() (net.Listener, error) {
	return winio.ListenPipe(socketName(), &winio.PipeConfig{
		SecurityDescriptor: pipeSecurity,
		MessageMode:        false,
	})
}

type handler struct{ t *tunnel }

// The Service Control Manager's side of the contract. Stop and Shutdown take
// the tunnel down; everything else is accepted and ignored, which is what the
// SCM expects.
func (h handler) Execute(_ []string, r <-chan svc.ChangeRequest, s chan<- svc.Status) (bool, uint32) {
	const accepted = svc.AcceptStop | svc.AcceptShutdown
	s <- svc.Status{State: svc.StartPending}

	go func() {
		// **Resume at start, so a machine that has joined before is on its
		// network before anyone logs in.** It creates nothing: with no key,
		// the library is given the identity already on disk, and with no
		// identity it refuses rather than enrolling.
		if err := h.t.start("", ""); err != nil {
			log.Printf("onv-tunnel: nothing to resume: %v", err)
		}
	}()
	go func() {
		if err := serve(h.t); err != nil {
			log.Printf("onv-tunnel: serve: %v", err)
		}
	}()

	s <- svc.Status{State: svc.Running, Accepts: accepted}
	for c := range r {
		switch c.Cmd {
		case svc.Interrogate:
			s <- c.CurrentStatus
		case svc.Stop, svc.Shutdown:
			s <- svc.Status{State: svc.StopPending}
			_ = h.t.stop()
			return false, 0
		}
	}
	return false, 0
}

// The rule's name, checked as well as created: a rule by this name pointing at
// some other binary is a rule from a previous install location, and leaving it
// would permit the wrong program while this one stays blocked.
const firewallRule = "Omnuv private network"

// ensureFirewallRule lets peers reach this daemon.
//
// **Why the tunnel needs one when the client already has one.** The installer
// creates a firewall exception for `OmnuvClient.exe`, which streams — but ICE
// is done by *this* process, and nothing anywhere creates a rule for it. With
// all three profiles on, inbound UDP to the tunnel is dropped, every
// connectivity check times out, and the peers fall back to a relay. Measured on
// 16 September: a buyer's stream ran at 60 fps through a server in another
// country while both machines sat in the same city.
//
// **Scoped by program, never by port.** The library picks its own port and may
// pick a different one tomorrow; a rule naming the binary covers whatever it
// binds, and needs no adjustment when it changes. A per-port rule is the shape
// that would need maintaining, which is the shape to avoid.
//
// **Asserted at every start rather than installed once.** This is the same
// reasoning the provider agent follows for a machine's configuration: a rule
// somebody removed, or an install that never made one, is repaired by the next
// start instead of being discovered by a buyer whose stream is slow. It costs
// two short commands.
//
// Never fatal. Creating a rule needs elevation, and this daemon is run by hand
// on a lab rig as an ordinary user; a tunnel that refuses to start for want of
// a firewall rule is worse than one that starts and says it could not.
func ensureFirewallRule() {
	exe, err := os.Executable()
	if err != nil {
		log.Printf("onv-tunnel: firewall: cannot find my own path: %v", err)
		return
	}

	// `show rule` names the program only with `verbose`, and its exit status is
	// 1 when no such rule exists — which is an answer, not a failure.
	out, _ := exec.Command("netsh", "advfirewall", "firewall", "show", "rule",
		"name="+firewallRule, "verbose").CombinedOutput()
	if strings.Contains(strings.ToLower(string(out)), strings.ToLower(exe)) {
		return
	}

	// A rule with our name but another program is stale; removing it first is
	// what makes this converge rather than accumulate.
	if strings.Contains(string(out), firewallRule) {
		_ = exec.Command("netsh", "advfirewall", "firewall", "delete", "rule",
			"name="+firewallRule).Run()
		log.Printf("onv-tunnel: firewall: replaced a rule named %q that pointed elsewhere", firewallRule)
	}

	add, err := exec.Command("netsh", "advfirewall", "firewall", "add", "rule",
		"name="+firewallRule, "dir=in", "action=allow",
		"program="+exe, "enable=yes", "profile=any").CombinedOutput()
	if err != nil {
		// The message matters: "requires elevation" and "already exists" are
		// different problems and only one of them is ours.
		log.Printf("onv-tunnel: firewall: could not allow inbound to %s: %v: %s",
			exe, err, strings.TrimSpace(string(add)))
		return
	}
	log.Printf("onv-tunnel: firewall: inbound allowed for %s", exe)
}

func run(t *tunnel) error {
	// Before anything joins: a peer that comes up without this reaches its
	// peers through a relay, and nothing in the join reports that.
	ensureFirewallRule()

	// **Asked, not assumed.** The same binary is run by hand on the rig and by
	// the SCM in production, and calling svc.Run outside a service context
	// fails with an error that explains nothing.
	isService, err := svc.IsWindowsService()
	if err != nil {
		return err
	}
	if !isService {
		go func() {
			if err := t.start("", ""); err != nil {
				log.Printf("onv-tunnel: nothing to resume: %v", err)
			}
		}()
		return serve(t)
	}
	return svc.Run(serviceName, handler{t: t})
}
