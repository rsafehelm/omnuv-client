package main

// Removing a mapping NetBird left on the router.
//
// NetBird deletes its mapping when the engine stops (`manager.go:319`), and
// for two of its three protocols that is the whole story. Two cases are not:
//
//	NAT-PMP   go-nat's delete sends nothing (go-nat natpmp.go:125-128: it
//	          forgets the port and returns nil), so a graceful stop leaves
//	          the mapping on the router for the rest of its two hours, while
//	          NetBird logs "deleted". Sent here, as RFC 6886 §3.4 defines it:
//	          the internal port, external port 0, lifetime 0. Stateless, so it
//	          works after a crash too.
//	a crash   the process that made a UPnP mapping held its external port in
//	          memory (go-nat upnp.go:548-571 deletes only what its own cache
//	          names), and a UPnP-725 router holds it with no lease at all
//	          (`manager.go:187`). The record (portmap_observe.go) names the
//	          external port; the router is asked what it holds there, and the
//	          entry is deleted only when it is this machine's, for this port,
//	          described "NetBird" — never a mapping somebody else made.
//
// PCP cannot be removed after a crash: the server matches a delete by a nonce
// that lived in the dead process's memory (go-nat pcp/client.go:253-263). It
// expires with its lease, and the report says when.
//
// The libraries are the ones NetBird's own mapper uses, at the versions its
// module resolves (go.mod): jackpal/go-nat-pmp v1.0.2 (Apache-2.0),
// huin/goupnp v1.2.0 (BSD-2-Clause), libp2p/go-netroute v0.4.0 (BSD-3-Clause).

import (
	"context"
	"errors"
	"fmt"
	"net"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/huin/goupnp/dcps/internetgateway1"
	"github.com/huin/goupnp/dcps/internetgateway2"
	natpmp "github.com/jackpal/go-nat-pmp"
	"github.com/libp2p/go-netroute"
)

// NetBird's description on every mapping it makes (`manager.go:21`).
const netbirdMappingDescription = "NetBird"

// UPnP error 714 in a SOAP fault, the way NetBird matches 725 (`manager.go:26`).
var upnpErrNoSuchEntry = regexp.MustCompile(`<errorCode>\s*714\s*</errorCode>`)

// As variables so the tests can stand in for a router.
var (
	releaseNATPMP = natpmpDelete
	releaseUPnP   = upnpDeleteOwned
)

// Whatever the record names is removed from the router, and the observer
// told what became of it. Answers the sentence for the report; empty when
// nothing was recorded.
func (o *portmapObserver) settle(ctx context.Context, now time.Time) string {
	rec, _ := readLeaseRecord(o.recordPath)
	said, gone := releaseLeftover(ctx, o.recordPath, now)
	if rec != nil && said != "" {
		if gone {
			o.released(*rec)
		} else {
			o.releaseFailed(*rec, said)
		}
	}
	return said
}

// Remove the recorded mapping from the router, if one is recorded. Answers
// what happened in a sentence for the report, and whether the record is gone.
func releaseLeftover(ctx context.Context, path string, now time.Time) (string, bool) {
	rec, err := readLeaseRecord(path)
	if err != nil {
		// An unreadable record names nothing that could be safely deleted.
		_ = removeLeaseRecord(path)
		return err.Error() + "; discarded", true
	}
	if rec == nil {
		return "", true
	}
	gateway := rec.Gateway
	switch {
	case strings.Contains(gateway, "NAT-PMP"):
		err = releaseNATPMP(ctx, *rec)
	case strings.HasPrefix(strings.ToUpper(gateway), "UPNP"):
		err = releaseUPnP(ctx, *rec)
	default:
		// PCP, or a protocol this file does not know: nothing can be sent.
		if expires := rec.expires(); !expires.IsZero() && now.After(expires) {
			_ = removeLeaseRecord(path)
			return fmt.Sprintf("%s mapping of port %d expired on the router at %s", gateway, rec.InternalPort, expires.Format(time.RFC3339)), true
		}
		return fmt.Sprintf("%s mapping of port %d cannot be removed after a restart; it expires at %s", gateway, rec.InternalPort, rec.expires().Format(time.RFC3339)), false
	}
	if err != nil {
		return fmt.Sprintf("%s mapping of port %d is still on the router: %v", gateway, rec.InternalPort, err), false
	}
	if err := removeLeaseRecord(path); err != nil {
		return "removed from the router, but the record stays: " + err.Error(), false
	}
	return fmt.Sprintf("%s mapping of port %d removed from the router", gateway, rec.InternalPort), true
}

func defaultGateway() (net.IP, error) {
	router, err := netroute.New()
	if err != nil {
		return nil, err
	}
	// go-nat's own probe address (gateway.go:42-47): netlink refuses 0.0.0.0.
	dst := net.IPv4zero
	if runtime.GOOS == "linux" {
		dst = net.IPv4(0, 0, 0, 1)
	}
	_, gw, _, err := router.Route(dst)
	if err != nil {
		return nil, err
	}
	if gw == nil {
		return nil, errors.New("no default gateway")
	}
	return gw, nil
}

func natpmpDelete(ctx context.Context, rec leaseRecord) error {
	gw, err := defaultGateway()
	if err != nil {
		return fmt.Errorf("find the router: %w", err)
	}
	timeout := 4 * time.Second
	if deadline, ok := ctx.Deadline(); ok && time.Until(deadline) < timeout {
		timeout = time.Until(deadline)
	}
	result, err := natpmp.NewClientWithTimeout(gw, timeout).AddPortMapping(rec.Protocol, rec.InternalPort, 0, 0)
	if err != nil {
		return err
	}
	if result.PortMappingLifetimeInSeconds != 0 {
		return fmt.Errorf("the router answered a lifetime of %ds to a delete", result.PortMappingLifetimeInSeconds)
	}
	return nil
}

// The two calls every UPnP WAN connection service has, in both generations.
type upnpConnection interface {
	GetSpecificPortMappingEntryCtx(ctx context.Context, remoteHost string, externalPort uint16, protocol string) (uint16, string, bool, string, uint32, error)
	DeletePortMappingCtx(ctx context.Context, remoteHost string, externalPort uint16, protocol string) error
}

func upnpConnections(ctx context.Context) []upnpConnection {
	var (
		mu  sync.Mutex
		all []upnpConnection
		wg  sync.WaitGroup
	)
	keep := func(found []upnpConnection) {
		mu.Lock()
		all = append(all, found...)
		mu.Unlock()
	}
	searches := []func(){
		func() { c, _, _ := internetgateway2.NewWANIPConnection2ClientsCtx(ctx); keep(asConnections(c)) },
		func() { c, _, _ := internetgateway2.NewWANIPConnection1ClientsCtx(ctx); keep(asConnections(c)) },
		func() { c, _, _ := internetgateway2.NewWANPPPConnection1ClientsCtx(ctx); keep(asConnections(c)) },
		func() { c, _, _ := internetgateway1.NewWANIPConnection1ClientsCtx(ctx); keep(asConnections(c)) },
		func() { c, _, _ := internetgateway1.NewWANPPPConnection1ClientsCtx(ctx); keep(asConnections(c)) },
	}
	for _, search := range searches {
		wg.Add(1)
		go func() { defer wg.Done(); search() }()
	}
	wg.Wait()
	return all
}

func asConnections[T upnpConnection](found []T) []upnpConnection {
	out := make([]upnpConnection, 0, len(found))
	for _, c := range found {
		out = append(out, c)
	}
	return out
}

func localAddresses() map[string]bool {
	out := map[string]bool{}
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return out
	}
	for _, a := range addrs {
		if n, ok := a.(*net.IPNet); ok {
			out[n.IP.String()] = true
		}
	}
	return out
}

func upnpDeleteOwned(ctx context.Context, rec leaseRecord) error {
	if rec.ExternalPort <= 0 || rec.ExternalPort > 65535 {
		return errors.New("the record names no external port")
	}
	conns := upnpConnections(ctx)
	if len(conns) == 0 {
		return errors.New("no UPnP router answered")
	}
	return deleteOwnedUPnP(ctx, conns, rec, localAddresses())
}

// Split out so the ownership rule can be tested without a router.
func deleteOwnedUPnP(ctx context.Context, conns []upnpConnection, rec leaseRecord, mine map[string]bool) error {
	proto := strings.ToUpper(rec.Protocol)
	var errs []error
	for _, c := range conns {
		internalPort, client, _, description, _, err := c.GetSpecificPortMappingEntryCtx(ctx, "", uint16(rec.ExternalPort), proto)
		if err != nil {
			// UPnP error 714, NoSuchEntryInArray: already gone.
			if upnpErrNoSuchEntry.MatchString(err.Error()) {
				return nil
			}
			errs = append(errs, err)
			continue
		}
		if int(internalPort) != rec.InternalPort || description != netbirdMappingDescription || !mine[client] {
			return fmt.Errorf("external port %d is held by %s:%d (%q), not this machine's mapping; left alone", rec.ExternalPort, client, internalPort, description)
		}
		return c.DeletePortMappingCtx(ctx, "", uint16(rec.ExternalPort), proto)
	}
	return errors.Join(errs...)
}
