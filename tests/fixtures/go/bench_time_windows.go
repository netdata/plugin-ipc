//go:build windows

package main

import wintransport "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"

func selfCPUSeconds() float64 {
	return wintransport.SelfCPUSeconds()
}
