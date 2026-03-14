//go:build windows

package main

import wintransport "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"

func defaultCgroupsSupportedProfiles() uint32 {
	return wintransport.ProfileNamedPipe
}

func defaultCgroupsPreferredProfiles() uint32 {
	return wintransport.ProfileNamedPipe
}
