package protocol

import "testing"

func labels(items ...struct{ Key, Value []byte }) []struct{ Key, Value []byte } {
	return items
}

func TestCgroupsLookupRoundTrip(t *testing.T) {
	var req [256]byte
	n, err := EncodeCgroupsLookupRequest([][]byte{
		[]byte("/sys/fs/cgroup/a"),
		[]byte("/system.slice/docker-abc.scope"),
	}, req[:])
	if err != nil {
		t.Fatalf("encode request: %v", err)
	}
	reqView, err := DecodeCgroupsLookupRequest(req[:n])
	if err != nil {
		t.Fatalf("decode request: %v", err)
	}
	if reqView.ItemCount != 2 {
		t.Fatalf("item count = %d, want 2", reqView.ItemCount)
	}
	item0, err := reqView.Item(0)
	if err != nil || item0.String() != "/sys/fs/cgroup/a" {
		t.Fatalf("item 0 = %q, err=%v", item0.String(), err)
	}

	var resp [1024]byte
	builder := NewCgroupsLookupBuilder(resp[:], 2, 123)
	if err := builder.Add(
		CgroupLookupKnown,
		OrchestratorK8s,
		[]byte("/sys/fs/cgroup/a"),
		[]byte("pod-a"),
		labels(struct{ Key, Value []byte }{[]byte("namespace"), []byte("default")}),
	); err != nil {
		t.Fatalf("add known: %v", err)
	}
	if err := builder.Add(
		CgroupLookupUnknownPermanent,
		0,
		[]byte("/system.slice/docker-abc.scope"),
		nil,
		nil,
	); err != nil {
		t.Fatalf("add unknown: %v", err)
	}
	total := builder.Finish()
	view, err := DecodeCgroupsLookupResponse(resp[:total])
	if err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if view.ItemCount != 2 || view.Generation != 123 {
		t.Fatalf("header = count %d generation %d", view.ItemCount, view.Generation)
	}
	got, err := view.Item(0)
	if err != nil {
		t.Fatalf("response item 0: %v", err)
	}
	if got.Status != CgroupLookupKnown || got.Orchestrator != OrchestratorK8s ||
		got.Path.String() != "/sys/fs/cgroup/a" || got.Name.String() != "pod-a" ||
		got.LabelCount != 1 {
		t.Fatalf("bad known item: %+v", got)
	}
	label, err := got.Label(0)
	if err != nil {
		t.Fatalf("label 0: %v", err)
	}
	if label.Key.String() != "namespace" || label.Value.String() != "default" {
		t.Fatalf("bad label: %q=%q", label.Key.String(), label.Value.String())
	}
}

func TestAppsLookupRoundTrip(t *testing.T) {
	var req [128]byte
	n, err := EncodeAppsLookupRequest([]uint32{0, 1234, 9999}, req[:])
	if err != nil {
		t.Fatalf("encode request: %v", err)
	}
	reqView, err := DecodeAppsLookupRequest(req[:n])
	if err != nil {
		t.Fatalf("decode request: %v", err)
	}
	pid, err := reqView.Item(0)
	if err != nil || pid != 0 {
		t.Fatalf("item 0 pid = %d, err=%v", pid, err)
	}

	var resp [2048]byte
	builder := NewAppsLookupBuilder(resp[:], 3, 77)
	if err := builder.Add(
		PidLookupKnown,
		AppsCgroupKnown,
		OrchestratorDocker,
		1234,
		1,
		1000,
		^uint64(0),
		[]byte("nginx"),
		[]byte("/docker/abc"),
		[]byte("container-a"),
		labels(struct{ Key, Value []byte }{[]byte("image"), []byte("nginx:latest")}),
	); err != nil {
		t.Fatalf("add known: %v", err)
	}
	if err := builder.Add(
		PidLookupKnown,
		AppsCgroupHostRoot,
		0,
		0,
		0,
		0,
		0,
		[]byte("swapper"),
		nil,
		nil,
		nil,
	); err != nil {
		t.Fatalf("add host root: %v", err)
	}
	if err := builder.Add(
		PidLookupUnknown,
		AppsCgroupKnown,
		0,
		9999,
		0,
		NipcUIDUnset,
		0,
		nil,
		nil,
		nil,
		nil,
	); err != nil {
		t.Fatalf("add unknown: %v", err)
	}
	total := builder.Finish()
	view, err := DecodeAppsLookupResponse(resp[:total])
	if err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if view.ItemCount != 3 || view.Generation != 77 {
		t.Fatalf("header = count %d generation %d", view.ItemCount, view.Generation)
	}
	item0, err := view.Item(0)
	if err != nil {
		t.Fatalf("item 0: %v", err)
	}
	if item0.Pid != 1234 || item0.Status != PidLookupKnown ||
		item0.CgroupStatus != AppsCgroupKnown ||
		item0.Comm.String() != "nginx" ||
		item0.CgroupPath.String() != "/docker/abc" ||
		item0.Starttime != ^uint64(0) {
		t.Fatalf("bad known item: %+v", item0)
	}
	item1, err := view.Item(1)
	if err != nil {
		t.Fatalf("item 1: %v", err)
	}
	if item1.Pid != 0 || item1.CgroupStatus != AppsCgroupHostRoot || item1.CgroupPath.Len() != 0 {
		t.Fatalf("bad host-root item: %+v", item1)
	}
	item2, err := view.Item(2)
	if err != nil {
		t.Fatalf("item 2: %v", err)
	}
	if item2.Pid != 9999 || item2.Status != PidLookupUnknown || item2.Uid != NipcUIDUnset {
		t.Fatalf("bad unknown item: %+v", item2)
	}
}

func TestLookupValidationEdges(t *testing.T) {
	var req [128]byte
	if _, err := EncodeCgroupsLookupRequest([][]byte{[]byte("bad\x00path")}, req[:]); err != ErrBadLayout {
		t.Fatalf("interior NUL request error = %v, want ErrBadLayout", err)
	}

	var resp [256]byte
	builder := NewAppsLookupBuilder(resp[:], 1, 0)
	if err := builder.Add(
		PidLookupKnown,
		AppsCgroupHostRoot,
		0,
		1,
		0,
		0,
		1,
		[]byte("1234567890123456"),
		nil,
		nil,
		nil,
	); err != ErrBadLayout {
		t.Fatalf("comm len 16 error = %v, want ErrBadLayout", err)
	}

	cg := NewCgroupsLookupBuilder(resp[:], 1, 0)
	if err := cg.Add(CgroupLookupKnown, 99, []byte("/x"), nil, nil); err != nil {
		t.Fatalf("unknown orchestrator should be accepted: %v", err)
	}
	total := cg.Finish()
	itemStart := CgroupsLookupRespHdr + LookupDirEntrySize + int(ne.Uint32(resp[CgroupsLookupRespHdr:CgroupsLookupRespHdr+4]))
	ne.PutUint16(resp[itemStart+2:itemStart+4], 99)
	if _, err := DecodeCgroupsLookupResponse(resp[:total]); err != ErrBadLayout {
		t.Fatalf("bad status error = %v, want ErrBadLayout", err)
	}
}

func TestLookupDispatchRejectsShortResponseBuffer(t *testing.T) {
	var req [128]byte
	reqLen, err := EncodeCgroupsLookupRequest([][]byte{[]byte("/x")}, req[:])
	if err != nil {
		t.Fatalf("encode cgroups request: %v", err)
	}
	shortCgroups := make([]byte, CgroupsLookupRespHdr+LookupDirEntrySize-1)
	n, err := DispatchCgroupsLookup(req[:reqLen], shortCgroups, func(*CgroupsLookupRequestView, *CgroupsLookupBuilder) bool {
		t.Fatal("handler should not run with undersized response buffer")
		return false
	})
	if err != ErrOverflow || n != 0 {
		t.Fatalf("cgroups dispatch = n %d err %v, want 0 ErrOverflow", n, err)
	}

	reqLen, err = EncodeAppsLookupRequest([]uint32{1234}, req[:])
	if err != nil {
		t.Fatalf("encode apps request: %v", err)
	}
	shortApps := make([]byte, AppsLookupRespHdr+LookupDirEntrySize-1)
	n, err = DispatchAppsLookup(req[:reqLen], shortApps, func(*AppsLookupRequestView, *AppsLookupBuilder) bool {
		t.Fatal("handler should not run with undersized response buffer")
		return false
	})
	if err != ErrOverflow || n != 0 {
		t.Fatalf("apps dispatch = n %d err %v, want 0 ErrOverflow", n, err)
	}
}

func TestLookupDecodeRejectsMaxUint32DirectoryOffset(t *testing.T) {
	var resp [512]byte
	cg := NewCgroupsLookupBuilder(resp[:], 1, 0)
	if err := cg.Add(CgroupLookupKnown, OrchestratorDocker, []byte("/x"), nil, nil); err != nil {
		t.Fatalf("add cgroups item: %v", err)
	}
	total := cg.Finish()
	ne.PutUint32(resp[CgroupsLookupRespHdr:CgroupsLookupRespHdr+4], ^uint32(0)-7)
	if _, err := DecodeCgroupsLookupResponse(resp[:total]); err != ErrOutOfBounds {
		t.Fatalf("cgroups response with max offset error = %v, want ErrOutOfBounds", err)
	}

	apps := NewAppsLookupBuilder(resp[:], 1, 0)
	if err := apps.Add(
		PidLookupKnown,
		AppsCgroupHostRoot,
		0,
		1234,
		1,
		1000,
		42,
		[]byte("nginx"),
		nil,
		nil,
		nil,
	); err != nil {
		t.Fatalf("add apps item: %v", err)
	}
	total = apps.Finish()
	ne.PutUint32(resp[AppsLookupRespHdr:AppsLookupRespHdr+4], ^uint32(0)-7)
	if _, err := DecodeAppsLookupResponse(resp[:total]); err != ErrOutOfBounds {
		t.Fatalf("apps response with max offset error = %v, want ErrOutOfBounds", err)
	}
}

func TestLookupLabelLayoutOverflow(t *testing.T) {
	sample := labels(struct{ Key, Value []byte }{[]byte("k"), []byte("v")})
	maxInt := int(^uint(0) >> 1)

	if _, _, _, err := labelLayoutGo(maxInt-3, sample); err != ErrOverflow {
		t.Fatalf("align overflow error = %v, want ErrOverflow", err)
	}
	if _, _, _, err := labelLayoutGo(maxInt-15, sample); err != ErrOverflow {
		t.Fatalf("label table overflow error = %v, want ErrOverflow", err)
	}
}
