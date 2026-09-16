package main

import (
	"log"
	"net"
	"os"
	"path/filepath"

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

func run(t *tunnel) error {
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
