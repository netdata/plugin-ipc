package main

import (
	"errors"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroupssnapshot"
)

const (
	requestPayloadLimit  = protocol.CgroupsSnapshotRequestPayloadLen
	responsePayloadLimit = cgroupssnapshot.DefaultMaxResponsePayloadBytes
	responseBatchLimit   = cgroupssnapshot.DefaultMaxResponseBatchItems
)

func requestMessageCapacity() int {
	size, err := protocol.MaxBatchTotalSize(protocol.CgroupsSnapshotRequestPayloadLen, 1)
	if err != nil {
		panic(err)
	}
	return size
}

func validateCgroupsSnapshotRequest(message []byte, messageLen int) (protocol.MessageHeader, error) {
	header, err := protocol.DecodeMessageHeader(message[:messageLen])
	if err != nil {
		return protocol.MessageHeader{}, err
	}
	if header.Kind != protocol.MessageKindRequest ||
		header.Flags != 0 ||
		header.Code != protocol.MethodCgroupsSnapshot ||
		header.TransportStatus != protocol.TransportStatusOK ||
		header.ItemCount != 1 ||
		header.PayloadLen != protocol.CgroupsSnapshotRequestPayloadLen {
		return protocol.MessageHeader{}, errors.New("invalid cgroups request envelope")
	}
	if protocol.MessageHeaderLen+int(header.PayloadLen) != messageLen {
		return protocol.MessageHeader{}, errors.New("invalid cgroups request length")
	}
	if _, err := protocol.DecodeCgroupsSnapshotRequestView(message[protocol.MessageHeaderLen:messageLen]); err != nil {
		return protocol.MessageHeader{}, err
	}
	return header, nil
}
