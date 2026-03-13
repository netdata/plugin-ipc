//go:build windows

package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"syscall"
	"time"

	wintransport "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

func applyWindowsCgroupsTransportProfiles(config *wintransport.Config) {
	config.SupportedProfiles = parseEnvU32("NETIPC_SUPPORTED_PROFILES", config.SupportedProfiles)
	config.PreferredProfiles = parseEnvU32("NETIPC_PREFERRED_PROFILES", config.PreferredProfiles)
}

func serverReceiveTimeout() time.Duration {
	if value := os.Getenv("NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS"); value != "" {
		if ms, err := strconv.ParseInt(value, 10, 64); err == nil && ms > 0 {
			return time.Duration(ms) * time.Millisecond
		}
	}
	return 10 * time.Second
}

func runCgroupsServerOnce(serviceNamespace, serviceName string, authToken uint64) {
	config := wintransport.NewConfig(serviceNamespace, serviceName)
	applyWindowsCgroupsTransportProfiles(&config)
	config.MaxRequestPayloadBytes = requestPayloadBytes()
	config.MaxRequestBatchItems = 1
	config.MaxResponsePayloadBytes = responsePayloadBytes()
	config.MaxResponseBatchItems = responseBatchItems()
	config.AuthToken = authToken

	server, err := wintransport.Listen(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server-once failed: %v\n", err)
		os.Exit(1)
	}
	defer server.Close()

	if err := server.Accept(10 * time.Second); err != nil {
		fmt.Fprintf(os.Stderr, "server-once failed: %v\n", err)
		os.Exit(1)
	}

	request := make([]byte, requestMessageCapacity())
	requestLen, err := server.ReceiveMessage(request, 10*time.Second)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server-once failed: %v\n", err)
		os.Exit(1)
	}

	header, err := validateCgroupsSnapshotRequest(request, requestLen)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server-once failed: %v\n", err)
		os.Exit(1)
	}

	if err := server.SendMessage(fixedCgroupsResponse(header.MessageID), 10*time.Second); err != nil {
		fmt.Fprintf(os.Stderr, "server-once failed: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("CGROUPS_SERVER\t%d\t2\n", header.MessageID)
}

func runCgroupsServerLoop(serviceNamespace, serviceName string, maxRequests uint64, authToken uint64) {
	config := wintransport.NewConfig(serviceNamespace, serviceName)
	applyWindowsCgroupsTransportProfiles(&config)
	config.MaxRequestPayloadBytes = requestPayloadBytes()
	config.MaxRequestBatchItems = 1
	config.MaxResponsePayloadBytes = responsePayloadBytes()
	config.MaxResponseBatchItems = responseBatchItems()
	config.AuthToken = authToken

	server, err := wintransport.Listen(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server-loop failed: %v\n", err)
		os.Exit(1)
	}
	defer server.Close()

	if err := server.Accept(10 * time.Second); err != nil {
		fmt.Fprintf(os.Stderr, "server-loop failed: %v\n", err)
		os.Exit(1)
	}

	request := make([]byte, requestMessageCapacity())
	handled := uint64(0)
	for maxRequests == 0 || handled < maxRequests {
		requestLen, err := server.ReceiveMessage(request, serverReceiveTimeout())
		if err != nil {
			if isDisconnectError(err) {
				break
			}
			if maxRequests == 0 && handled > 0 && isIdleTimeoutError(err) {
				break
			}
			fmt.Fprintf(os.Stderr, "server-loop failed: %v\n", err)
			os.Exit(1)
		}

		header, err := validateCgroupsSnapshotRequest(request, requestLen)
		if err != nil {
			fmt.Fprintf(os.Stderr, "server-loop failed: %v\n", err)
			os.Exit(1)
		}

		if err := server.SendMessage(fixedCgroupsResponse(header.MessageID), 10*time.Second); err != nil {
			fmt.Fprintf(os.Stderr, "server-loop failed: %v\n", err)
			os.Exit(1)
		}
		handled++
	}

	fmt.Printf("CGROUPS_SERVER_LOOP\t%d\n", handled)
}

func requestPayloadBytes() uint32 {
	return requestPayloadLimit
}

func responsePayloadBytes() uint32 {
	return responsePayloadLimit
}

func responseBatchItems() uint32 {
	return responseBatchLimit
}

func isDisconnectError(err error) bool {
	var errno syscall.Errno

	return errors.Is(err, syscall.EPIPE) ||
		errors.Is(err, syscall.ENOTCONN) ||
		errors.Is(err, syscall.ECONNRESET) ||
		errors.Is(err, syscall.EPROTO) ||
		(errors.As(err, &errno) &&
			(errno == syscall.Errno(109) || errno == syscall.Errno(232) || errno == syscall.Errno(233))) ||
		errors.Is(err, io.EOF)
}

func isIdleTimeoutError(err error) bool {
	return errors.Is(err, os.ErrDeadlineExceeded) || os.IsTimeout(err)
}
