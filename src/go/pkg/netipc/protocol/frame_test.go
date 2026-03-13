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
	buf := make([]byte, 512)
	builder, err := NewCgroupsSnapshotResponseBuilder(buf, 42, true, 3, 2)
	if err != nil {
		t.Fatalf("NewCgroupsSnapshotResponseBuilder() error = %v", err)
	}
	if err := builder.AddItem(CgroupsSnapshotItem{
		Hash:    123,
		Options: 0x2,
		Enabled: true,
		Name:    "system.slice-nginx",
		Path:    "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs",
	}); err != nil {
		t.Fatalf("AddItem(first) error = %v", err)
	}
	if err := builder.AddItem(CgroupsSnapshotItem{
		Hash:    456,
		Options: 0x4,
		Enabled: false,
		Name:    "docker-1234",
		Path:    "",
	}); err != nil {
		t.Fatalf("AddItem(second) error = %v", err)
	}
	payloadLen, err := builder.Finish()
	if err != nil {
		t.Fatalf("Finish() error = %v", err)
	}

	view, err := DecodeCgroupsSnapshotView(buf[:payloadLen], 2)
	if err != nil {
		t.Fatalf("DecodeCgroupsSnapshotView() error = %v", err)
	}
	if view.LayoutVersion != CgroupsSnapshotLayoutVersion || view.Flags != 3 || !view.SystemdEnabled || view.Generation != 42 || view.ItemCount != 2 {
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
