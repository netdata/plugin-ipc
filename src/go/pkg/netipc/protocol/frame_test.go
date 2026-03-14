package protocol

import "testing"

func TestMessageHeaderRoundTrip(t *testing.T) {
	encoded, err := EncodeMessageHeader(MessageHeader{
		Magic:           MessageMagic,
		Version:         MessageVersion,
		HeaderLen:       MessageHeaderLen,
		Kind:            MessageKindControl,
		Flags:           MessageFlagBatch,
		Code:            ControlHelloAck,
		TransportStatus: TransportStatusLimitExceeded,
		PayloadLen:      96,
		ItemCount:       4,
		MessageID:       77,
	})
	if err != nil {
		t.Fatalf("EncodeMessageHeader() error = %v", err)
	}

	header, err := DecodeMessageHeader(encoded[:])
	if err != nil {
		t.Fatalf("DecodeMessageHeader() error = %v", err)
	}

	if total, err := MessageTotalSize(header); err != nil || total != 128 {
		t.Fatalf("MessageTotalSize() = %d, %v, want 128, nil", total, err)
	}
	if header.Kind != MessageKindControl || header.Flags != MessageFlagBatch || header.Code != ControlHelloAck {
		t.Fatalf("unexpected decoded header: %+v", header)
	}
}

func TestItemRefsRoundTrip(t *testing.T) {
	refs := []ItemRef{
		{Offset: 8, Length: 13},
		{Offset: 24, Length: 21},
	}
	decoded, err := DecodeItemRefs(EncodeItemRefs(refs), len(refs))
	if err != nil {
		t.Fatalf("DecodeItemRefs() error = %v", err)
	}
	if len(decoded) != len(refs) || decoded[0] != refs[0] || decoded[1] != refs[1] {
		t.Fatalf("unexpected item refs: %+v", decoded)
	}
}

func TestHelloPayloadRoundTrip(t *testing.T) {
	payload := HelloPayload{
		LayoutVersion:           1,
		Flags:                   2,
		Supported:               3,
		Preferred:               4,
		MaxRequestPayloadBytes:  5,
		MaxRequestBatchItems:    6,
		MaxResponsePayloadBytes: 7,
		MaxResponseBatchItems:   8,
		AuthToken:               9,
		PacketSize:              10,
	}
	encoded := EncodeHelloPayload(payload)
	decoded, err := DecodeHelloPayload(encoded[:])
	if err != nil {
		t.Fatalf("DecodeHelloPayload() error = %v", err)
	}
	if decoded != payload {
		t.Fatalf("unexpected hello payload: %+v", decoded)
	}
}

func TestHelloAckPayloadRoundTrip(t *testing.T) {
	payload := HelloAckPayload{
		LayoutVersion:               1,
		Flags:                       2,
		ServerSupported:             3,
		Intersection:                4,
		Selected:                    5,
		AgreedMaxRequestPayload:     6,
		AgreedMaxRequestBatchItems:  7,
		AgreedMaxResponsePayload:    8,
		AgreedMaxResponseBatchItems: 9,
		AgreedPacketSize:            10,
	}
	encoded := EncodeHelloAckPayload(payload)
	decoded, err := DecodeHelloAckPayload(encoded[:])
	if err != nil {
		t.Fatalf("DecodeHelloAckPayload() error = %v", err)
	}
	if decoded != payload {
		t.Fatalf("unexpected hello ack payload: %+v", decoded)
	}
}

func TestChunkHeaderRoundTrip(t *testing.T) {
	encoded, err := EncodeChunkHeader(ChunkHeader{
		Magic:           ChunkMagic,
		Version:         ChunkVersion,
		Flags:           3,
		MessageID:       99,
		TotalMessageLen: 1234,
		ChunkIndex:      1,
		ChunkCount:      4,
		ChunkPayloadLen: 512,
	})
	if err != nil {
		t.Fatalf("EncodeChunkHeader() error = %v", err)
	}
	decoded, err := DecodeChunkHeader(encoded[:])
	if err != nil {
		t.Fatalf("DecodeChunkHeader() error = %v", err)
	}
	if decoded.Magic != ChunkMagic || decoded.Version != ChunkVersion || decoded.Flags != 3 ||
		decoded.MessageID != 99 || decoded.TotalMessageLen != 1234 || decoded.ChunkIndex != 1 ||
		decoded.ChunkCount != 4 || decoded.ChunkPayloadLen != 512 {
		t.Fatalf("unexpected chunk header: %+v", decoded)
	}
}

func TestCgroupsSnapshotRequestRoundTrip(t *testing.T) {
	encoded := EncodeCgroupsSnapshotRequestPayload(CgroupsSnapshotRequest{Flags: 7})
	decoded, err := DecodeCgroupsSnapshotRequestView(encoded[:])
	if err != nil {
		t.Fatalf("DecodeCgroupsSnapshotRequestView() error = %v", err)
	}
	if decoded.LayoutVersion != CgroupsSnapshotLayoutVersion || decoded.Flags != 7 {
		t.Fatalf("unexpected cgroups snapshot request view: %+v", decoded)
	}
}

func TestCgroupsSnapshotResponseRoundTrip(t *testing.T) {
	items := []CgroupsSnapshotItem{
		{Hash: 123, Options: 0x2, Enabled: true, Name: "system.slice-nginx", Path: "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs"},
		{Hash: 456, Options: 0x4, Enabled: false, Name: "docker-1234", Path: ""},
		{Hash: 789, Options: 0x6, Enabled: true, Name: "kubepods-burstable-pod01234567_89ab_cdef_0123_456789abcdef.slice", Path: "/sys/fs/cgroup/kubepods.slice/kubepods-burstable.slice/kubepods-burstable-pod01234567_89ab_cdef_0123_456789abcdef.slice/cgroup.procs"},
		{Hash: 1001, Options: 0x2, Enabled: true, Name: "system.slice-sshd.service", Path: "/sys/fs/cgroup/system.slice/sshd.service/cgroup.procs"},
		{Hash: 1002, Options: 0x2, Enabled: true, Name: "system.slice-docker.service", Path: "/sys/fs/cgroup/system.slice/docker.service/cgroup.procs"},
		{Hash: 1003, Options: 0x6, Enabled: true, Name: "user.slice-user-1000.slice-session-3.scope", Path: "/sys/fs/cgroup/user.slice/user-1000.slice/session-3.scope/cgroup.procs"},
		{Hash: 1004, Options: 0x2, Enabled: true, Name: "machine.slice-libvirt-qemu-5-win11.scope", Path: "/sys/fs/cgroup/machine.slice/libvirt-qemu-5-win11.scope/cgroup.procs"},
		{Hash: 1005, Options: 0x8, Enabled: false, Name: "system.slice-telegraf.service", Path: "/sys/fs/cgroup/system.slice/telegraf.service/cgroup.procs"},
		{Hash: 1006, Options: 0x6, Enabled: true, Name: "podman-7f0c8e91f1ce55b0c3d1b5a4f6e8d9c0.scope", Path: "/sys/fs/cgroup/system.slice/podman-7f0c8e91f1ce55b0c3d1b5a4f6e8d9c0.scope/cgroup.procs"},
		{Hash: 1007, Options: 0x4, Enabled: true, Name: "init.scope", Path: "/sys/fs/cgroup/init.scope/cgroup.procs"},
		{Hash: 1008, Options: 0x6, Enabled: true, Name: "system.slice-containerd.service", Path: "/sys/fs/cgroup/system.slice/containerd.service/cgroup.procs"},
		{Hash: 1009, Options: 0x4, Enabled: true, Name: "machine.slice-systemd-nspawn-observability-lab.scope", Path: "/sys/fs/cgroup/machine.slice/systemd-nspawn-observability-lab.scope/cgroup.procs"},
		{Hash: 1010, Options: 0x6, Enabled: true, Name: "user.slice-user-1001.slice-user@1001.service-app.slice-observability-frontend.scope", Path: "/sys/fs/cgroup/user.slice/user-1001.slice/user@1001.service/app.slice/observability-frontend.scope/cgroup.procs"},
		{Hash: 1011, Options: 0x1, Enabled: false, Name: "crio-53d2b1b5d7a04d8f9e2f6a7b8c9d0e1f.scope", Path: "/sys/fs/cgroup/kubepods.slice/kubepods-pod98765432_10fe_dcba_9876_543210fedcba.slice/crio-53d2b1b5d7a04d8f9e2f6a7b8c9d0e1f.scope/cgroup.procs"},
		{Hash: 1012, Options: 0x2, Enabled: true, Name: "system.slice-netdata.service", Path: "/sys/fs/cgroup/system.slice/netdata.service/cgroup.procs"},
		{Hash: 1013, Options: 0x6, Enabled: true, Name: "system.slice-super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service", Path: "/sys/fs/cgroup/system.slice/super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service/cgroup.procs"},
	}
	bufLen := CgroupsSnapshotResponseHeaderLen + len(items)*MessageItemRefLen
	for idx, item := range items {
		start, err := AlignedItemSize(uint32(bufLen))
		if err != nil {
			t.Fatalf("AlignedItemSize(%d) error = %v", idx, err)
		}
		itemLen, err := CgroupsSnapshotItemPayloadLen(item)
		if err != nil {
			t.Fatalf("CgroupsSnapshotItemPayloadLen(%d) error = %v", idx, err)
		}
		bufLen = start + itemLen
	}
	buf := make([]byte, bufLen)
	builder, err := NewCgroupsSnapshotResponseBuilder(buf, 42, true, 3, uint32(len(items)))
	if err != nil {
		t.Fatalf("NewCgroupsSnapshotResponseBuilder() error = %v", err)
	}
	for idx, item := range items {
		if err := builder.AddItem(item); err != nil {
			t.Fatalf("AddItem(%d) error = %v", idx, err)
		}
	}
	payloadLen, err := builder.Finish()
	if err != nil {
		t.Fatalf("Finish() error = %v", err)
	}

	view, err := DecodeCgroupsSnapshotView(buf[:payloadLen], uint32(len(items)))
	if err != nil {
		t.Fatalf("DecodeCgroupsSnapshotView() error = %v", err)
	}
	if view.LayoutVersion != CgroupsSnapshotLayoutVersion || view.Flags != 3 || !view.SystemdEnabled || view.Generation != 42 || view.ItemCount != uint32(len(items)) {
		t.Fatalf("unexpected cgroups snapshot view: %+v", view)
	}

	first, err := view.ItemViewAt(0)
	if err != nil {
		t.Fatalf("ItemViewAt(0) error = %v", err)
	}
	if first.Hash != 123 || first.Options != 0x2 || !first.Enabled || first.NameView.CopyString() != "system.slice-nginx" || first.PathView.CopyString() != "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs" {
		t.Fatalf("unexpected first item view: %+v", first)
	}

	second, err := view.ItemViewAt(1)
	if err != nil {
		t.Fatalf("ItemViewAt(1) error = %v", err)
	}
	if second.Hash != 456 || second.Options != 0x4 || second.Enabled || second.NameView.CopyString() != "docker-1234" || !second.PathView.IsEmpty() {
		t.Fatalf("unexpected second item view: %+v", second)
	}

	longItem, err := view.ItemViewAt(uint32(len(items) - 1))
	if err != nil {
		t.Fatalf("ItemViewAt(last) error = %v", err)
	}
	if longItem.Hash != 1013 || longItem.NameView.CopyString() != "system.slice-super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service" {
		t.Fatalf("unexpected last item view: %+v", longItem)
	}
}

func TestIncrementRequestRoundTrip(t *testing.T) {
	frame := EncodeIncrementRequest(42, IncrementRequest{Value: 100})
	requestID, request, err := DecodeIncrementRequest(frame)
	if err != nil {
		t.Fatalf("DecodeIncrementRequest() error = %v", err)
	}
	if requestID != 42 || request.Value != 100 {
		t.Fatalf("unexpected decoded request: id=%d value=%d", requestID, request.Value)
	}
}

func TestIncrementResponseRoundTrip(t *testing.T) {
	frame := EncodeIncrementResponse(7, IncrementResponse{Status: StatusOK, Value: 8})
	requestID, response, err := DecodeIncrementResponse(frame)
	if err != nil {
		t.Fatalf("DecodeIncrementResponse() error = %v", err)
	}
	if requestID != 7 || response.Status != StatusOK || response.Value != 8 {
		t.Fatalf("unexpected decoded response: id=%d status=%d value=%d", requestID, response.Status, response.Value)
	}
}

func TestBatchSizeHelpers(t *testing.T) {
	aligned, err := AlignedItemSize(1024)
	if err != nil {
		t.Fatalf("AlignedItemSize(1024) error = %v", err)
	}
	if aligned != 1024 {
		t.Fatalf("AlignedItemSize(1024) = %d, want 1024", aligned)
	}

	aligned, err = AlignedItemSize(1025)
	if err != nil {
		t.Fatalf("AlignedItemSize(1025) error = %v", err)
	}
	if aligned != 1032 {
		t.Fatalf("AlignedItemSize(1025) = %d, want 1032", aligned)
	}

	payloadLen, err := MaxBatchPayloadLen(1024, 1)
	if err != nil {
		t.Fatalf("MaxBatchPayloadLen(1024,1) error = %v", err)
	}
	if payloadLen != 1032 {
		t.Fatalf("MaxBatchPayloadLen(1024,1) = %d, want 1032", payloadLen)
	}

	totalLen, err := MaxBatchTotalSize(1024, 1)
	if err != nil {
		t.Fatalf("MaxBatchTotalSize(1024,1) error = %v", err)
	}
	if totalLen != 1064 {
		t.Fatalf("MaxBatchTotalSize(1024,1) = %d, want 1064", totalLen)
	}

	payloadLen, err = MaxBatchPayloadLen(1025, 2)
	if err != nil {
		t.Fatalf("MaxBatchPayloadLen(1025,2) error = %v", err)
	}
	if payloadLen != 2080 {
		t.Fatalf("MaxBatchPayloadLen(1025,2) = %d, want 2080", payloadLen)
	}

	totalLen, err = MaxBatchTotalSize(1025, 2)
	if err != nil {
		t.Fatalf("MaxBatchTotalSize(1025,2) error = %v", err)
	}
	if totalLen != 2112 {
		t.Fatalf("MaxBatchTotalSize(1025,2) = %d, want 2112", totalLen)
	}
}

func TestRejectsInvalidMagic(t *testing.T) {
	frame := EncodeIncrementRequest(1, IncrementRequest{Value: 1})
	frame[offMagic] = 0
	if _, _, err := DecodeIncrementRequest(frame); err == nil {
		t.Fatal("DecodeIncrementRequest() unexpectedly succeeded")
	}
}

func fuzzSeedCgroupsSnapshotPayload(f *testing.F) {
	items := []CgroupsSnapshotItem{
		{Hash: 123, Options: 0x2, Enabled: true, Name: "system.slice-nginx", Path: "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs"},
		{Hash: 456, Options: 0x4, Enabled: false, Name: "docker-1234", Path: ""},
	}
	bufLen := CgroupsSnapshotResponseHeaderLen + len(items)*MessageItemRefLen
	for _, item := range items {
		start, err := AlignedItemSize(uint32(bufLen))
		if err != nil {
			f.Fatalf("AlignedItemSize() error = %v", err)
		}
		itemLen, err := CgroupsSnapshotItemPayloadLen(item)
		if err != nil {
			f.Fatalf("CgroupsSnapshotItemPayloadLen() error = %v", err)
		}
		bufLen = start + itemLen
	}
	buf := make([]byte, bufLen)
	builder, err := NewCgroupsSnapshotResponseBuilder(buf, 42, true, 3, uint32(len(items)))
	if err != nil {
		f.Fatalf("NewCgroupsSnapshotResponseBuilder() error = %v", err)
	}
	for idx, item := range items {
		if err := builder.AddItem(item); err != nil {
			f.Fatalf("AddItem(%d) error = %v", idx, err)
		}
	}
	payloadLen, err := builder.Finish()
	if err != nil {
		f.Fatalf("Finish() error = %v", err)
	}
	f.Add(buf[:payloadLen], uint32(len(items)))
}

func FuzzDecodeMessageHeader(f *testing.F) {
	encoded, err := EncodeMessageHeader(MessageHeader{
		Magic:           MessageMagic,
		Version:         MessageVersion,
		HeaderLen:       MessageHeaderLen,
		Kind:            MessageKindResponse,
		Flags:           MessageFlagBatch,
		Code:            MethodCgroupsSnapshot,
		TransportStatus: TransportStatusOK,
		PayloadLen:      64,
		ItemCount:       2,
		MessageID:       99,
	})
	if err != nil {
		f.Fatalf("EncodeMessageHeader() error = %v", err)
	}
	f.Add(encoded[:])
	f.Add([]byte{})
	f.Add([]byte{1, 2, 3})

	f.Fuzz(func(t *testing.T, data []byte) {
		header, err := DecodeMessageHeader(data)
		if err == nil {
			if _, err := MessageTotalSize(header); err != nil {
				t.Fatalf("MessageTotalSize() failed for decoded header: %v", err)
			}
		}
	})
}

func FuzzDecodeChunkHeader(f *testing.F) {
	encoded, err := EncodeChunkHeader(ChunkHeader{
		Magic:           ChunkMagic,
		Version:         ChunkVersion,
		Flags:           0,
		MessageID:       99,
		TotalMessageLen: 4096,
		ChunkIndex:      0,
		ChunkCount:      4,
		ChunkPayloadLen: 1024,
	})
	if err != nil {
		f.Fatalf("EncodeChunkHeader() error = %v", err)
	}
	f.Add(encoded[:])
	f.Add([]byte{})
	f.Add([]byte{1, 2, 3})

	f.Fuzz(func(t *testing.T, data []byte) {
		header, err := DecodeChunkHeader(data)
		if err == nil {
			if header.ChunkCount == 0 || header.ChunkIndex >= header.ChunkCount {
				t.Fatalf("DecodeChunkHeader() returned an invalid chunk header: %+v", header)
			}
			if header.ChunkPayloadLen == 0 || header.TotalMessageLen == 0 {
				t.Fatalf("DecodeChunkHeader() returned a zero-sized chunk header: %+v", header)
			}
		}
	})
}

func FuzzDecodeCgroupsSnapshotRequestView(f *testing.F) {
	encoded := EncodeCgroupsSnapshotRequestPayload(CgroupsSnapshotRequest{Flags: 0x1234})
	f.Add(encoded[:])
	f.Add([]byte{})
	f.Add([]byte{1, 2, 3})

	f.Fuzz(func(t *testing.T, data []byte) {
		view, err := DecodeCgroupsSnapshotRequestView(data)
		if err == nil && view.LayoutVersion != CgroupsSnapshotLayoutVersion {
			t.Fatalf("DecodeCgroupsSnapshotRequestView() returned an invalid layout version: %+v", view)
		}
	})
}

func FuzzDecodeCgroupsSnapshotView(f *testing.F) {
	fuzzSeedCgroupsSnapshotPayload(f)
	f.Add([]byte{}, uint32(0))
	f.Add([]byte{1, 2, 3}, uint32(1))

	f.Fuzz(func(t *testing.T, payload []byte, itemCount uint32) {
		if itemCount > 4096 {
			t.Skip()
		}
		view, err := DecodeCgroupsSnapshotView(payload, itemCount)
		if err == nil {
			for i := uint32(0); i < view.ItemCount; i++ {
				if _, err := view.ItemViewAt(i); err != nil {
					t.Fatalf("ItemViewAt(%d) failed after DecodeCgroupsSnapshotView() succeeded: %v", i, err)
				}
			}
		}
	})
}
