//go:build unix

package main

import posixtransport "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"

func defaultCgroupsSupportedProfiles() uint32 {
	return posixtransport.ProfileUDSSeqpacket
}

func defaultCgroupsPreferredProfiles() uint32 {
	return posixtransport.ProfileUDSSeqpacket
}
