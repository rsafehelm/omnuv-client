package main

import (
	"fmt"

	"golang.org/x/sys/windows"
)

// The DNS Client service's cache, cleared by the call `ipconfig /flushdns`
// makes: DnsFlushResolverCache in dnsapi.dll, which returns TRUE on success.
var dnsFlushResolverCache = windows.NewLazySystemDLL("dnsapi.dll").NewProc("DnsFlushResolverCache")

func flushSystemDNS() error {
	if err := dnsFlushResolverCache.Find(); err != nil {
		return err
	}
	r, _, err := dnsFlushResolverCache.Call()
	if r == 0 {
		return fmt.Errorf("DnsFlushResolverCache: %v", err)
	}
	return nil
}
