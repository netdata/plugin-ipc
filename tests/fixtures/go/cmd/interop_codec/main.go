// Encode/decode test messages to/from files for cross-language interop
// testing. Uses the exact same test data as the C and Rust interop binaries.
//
// Usage:
//
//	interop_codec encode <output_dir>   - encode all test messages to files
//	interop_codec decode <input_dir>    - decode files and verify correctness
//
// Returns 0 on success, 1 on failure.
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

func writeFile(dir, name string, data []byte) {
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, data, 0600); err != nil { // #nosec G703 -- interop fixture writes caller-selected golden-file directory.
		fmt.Fprintf(os.Stderr, "ERROR: cannot write %s: %v\n", path, err)
		os.Exit(1)
	}
}

func readFile(dir, name string) []byte {
	path := filepath.Join(dir, name)
	data, err := os.ReadFile(path) // #nosec G304,G703 -- interop fixture reads caller-selected golden-file directory.
	if err != nil {
		fmt.Fprintf(os.Stderr, "ERROR: cannot read %s: %v\n", path, err)
		os.Exit(1)
	}
	return data
}

type checker struct {
	pass int
	fail int
}

func (c *checker) check(cond bool, name string) {
	if cond {
		c.pass++
	} else {
		c.fail++
		fmt.Fprintf(os.Stderr, "FAIL: %s\n", name)
	}
}

func (c *checker) report(label string) bool {
	fmt.Printf("%s: %d passed, %d failed\n", label, c.pass, c.fail)
	return c.fail == 0
}

func doEncode(dir string) {
	// 1. Outer message header.
	{
		h := protocol.Header{
			Magic:           protocol.MagicMsg,
			Version:         protocol.Version,
			HeaderLen:       protocol.HeaderLen,
			Kind:            protocol.KindRequest,
			Flags:           protocol.FlagBatch,
			Code:            protocol.MethodCgroupsSnapshot,
			TransportStatus: protocol.StatusOK,
			PayloadLen:      12345,
			ItemCount:       42,
			MessageID:       0xDEADBEEFCAFEBABE,
		}
		var buf [32]byte
		h.Encode(buf[:])
		writeFile(dir, "header.bin", buf[:])
	}

	// 2. Chunk continuation header.
	{
		c := protocol.ChunkHeader{
			Magic:           protocol.MagicChunk,
			Version:         protocol.Version,
			Flags:           0,
			MessageID:       0x1234567890ABCDEF,
			TotalMessageLen: 100000,
			ChunkIndex:      3,
			ChunkCount:      10,
			ChunkPayloadLen: 8192,
		}
		var buf [32]byte
		c.Encode(buf[:])
		writeFile(dir, "chunk_header.bin", buf[:])
	}

	// 3. Hello payload.
	{
		h := protocol.Hello{
			LayoutVersion:           1,
			Flags:                   0,
			SupportedProfiles:       protocol.ProfileBaseline | protocol.ProfileSHMFutex,
			PreferredProfiles:       protocol.ProfileSHMFutex,
			MaxRequestPayloadBytes:  4096,
			MaxRequestBatchItems:    100,
			MaxResponsePayloadBytes: 1048576,
			MaxResponseBatchItems:   1,
			AuthToken:               0xAABBCCDDEEFF0011,
			PacketSize:              65536,
		}
		var buf [44]byte
		h.Encode(buf[:])
		writeFile(dir, "hello.bin", buf[:])
	}

	// 4. Hello-ack payload.
	{
		h := protocol.HelloAck{
			LayoutVersion:                 1,
			Flags:                         0,
			ServerSupportedProfiles:       0x07,
			IntersectionProfiles:          0x05,
			SelectedProfile:               protocol.ProfileSHMFutex,
			AgreedMaxRequestPayloadBytes:  2048,
			AgreedMaxRequestBatchItems:    50,
			AgreedMaxResponsePayloadBytes: 65536,
			AgreedMaxResponseBatchItems:   1,
			AgreedPacketSize:              32768,
			SessionID:                     1,
		}
		var buf [48]byte
		h.Encode(buf[:])
		writeFile(dir, "hello_ack.bin", buf[:])
	}

	// 5. Cgroups request.
	{
		r := protocol.CgroupsRequest{LayoutVersion: 1, Flags: 0}
		var buf [4]byte
		r.Encode(buf[:])
		writeFile(dir, "cgroups_req.bin", buf[:])
	}

	// 6. Cgroups snapshot response (multi-item).
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsBuilder(buf, 3, 1, 999)

		if err := b.Add(100, 0, 1,
			[]byte("init.scope"),
			[]byte("/sys/fs/cgroup/init.scope")); err != nil {
			fmt.Fprintf(os.Stderr, "ERROR: cannot add cgroup fixture: %v\n", err)
			os.Exit(1)
		}
		if err := b.Add(200, 0x02, 0,
			[]byte("system.slice/docker-abc.scope"),
			[]byte("/sys/fs/cgroup/system.slice/docker-abc.scope")); err != nil {
			fmt.Fprintf(os.Stderr, "ERROR: cannot add cgroup fixture: %v\n", err)
			os.Exit(1)
		}
		if err := b.Add(300, 0, 1, []byte(""), []byte("")); err != nil {
			fmt.Fprintf(os.Stderr, "ERROR: cannot add cgroup fixture: %v\n", err)
			os.Exit(1)
		}

		total := b.Finish()
		writeFile(dir, "cgroups_resp.bin", buf[:total])
	}

	// 7. Empty cgroups snapshot.
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsBuilder(buf, 0, 0, 42)
		total := b.Finish()
		writeFile(dir, "cgroups_resp_empty.bin", buf[:total])
	}

	// 8. CGROUPS_LOOKUP request variants.
	{
		buf := make([]byte, 8192)
		total, err := protocol.EncodeCgroupsLookupRequest(
			[][]byte{[]byte("/sys/fs/cgroup/a"), []byte("/system.slice/docker-abc.scope")},
			buf,
		)
		if err != nil {
			panic(err)
		}
		writeFile(dir, "cgroups_lookup_req.bin", buf[:total])

		total, err = protocol.EncodeCgroupsLookupRequest(nil, buf)
		if err != nil {
			panic(err)
		}
		writeFile(dir, "cgroups_lookup_req_empty.bin", buf[:total])
	}

	// 9. CGROUPS_LOOKUP response variants.
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsLookupBuilder(buf, 1, 100)
		labels := []struct{ Key, Value []byte }{
			{[]byte("namespace"), []byte("default")},
			{[]byte("pod"), []byte("web")},
		}
		if err := b.Add(protocol.CgroupLookupKnown, protocol.OrchestratorK8s,
			[]byte("/kubepods.slice/pod-a"), []byte("pod-a"), labels); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "cgroups_lookup_resp_known_with_labels.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsLookupBuilder(buf, 1, 101)
		if err := b.Add(protocol.CgroupLookupKnown, protocol.OrchestratorDocker,
			[]byte("/docker/abc"), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "cgroups_lookup_resp_known_no_labels.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsLookupBuilder(buf, 1, 102)
		if err := b.Add(protocol.CgroupLookupUnknownRetryLater, 0,
			[]byte("/missing/retry"), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "cgroups_lookup_resp_unknown_retry.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsLookupBuilder(buf, 1, 103)
		if err := b.Add(protocol.CgroupLookupUnknownPermanent, 0,
			[]byte("/gone"), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "cgroups_lookup_resp_unknown_permanent.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsLookupBuilder(buf, 1, 104)
		if err := b.Add(protocol.CgroupLookupPayloadExceeded, 0,
			[]byte("/payload-exceeded"), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "cgroups_lookup_resp_payload_exceeded.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsLookupBuilder(buf, 1, 105)
		if err := b.Add(protocol.CgroupLookupOversizedItem, 0,
			[]byte("/oversized"), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "cgroups_lookup_resp_oversized_item.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewCgroupsLookupBuilder(buf, 0, 106)
		total := b.Finish()
		writeFile(dir, "cgroups_lookup_resp_empty.bin", buf[:total])
	}

	// 10. APPS_LOOKUP request variants.
	{
		buf := make([]byte, 8192)
		total, err := protocol.EncodeAppsLookupRequest([]uint32{0, 1234, 4321}, buf)
		if err != nil {
			panic(err)
		}
		writeFile(dir, "apps_lookup_req.bin", buf[:total])

		total, err = protocol.EncodeAppsLookupRequest(nil, buf)
		if err != nil {
			panic(err)
		}
		writeFile(dir, "apps_lookup_req_empty.bin", buf[:total])
	}

	// 11. APPS_LOOKUP response variants.
	{
		buf := make([]byte, 8192)
		b := protocol.NewAppsLookupBuilder(buf, 1, 200)
		labels := []struct{ Key, Value []byte }{
			{[]byte("image"), []byte("nginx:latest")},
			{[]byte("service"), []byte("web")},
		}
		if err := b.Add(protocol.PidLookupKnown, protocol.AppsCgroupKnown,
			protocol.OrchestratorDocker, 1234, 1, 1000, 123456,
			[]byte("123456789012345"), []byte("/docker/abc"),
			[]byte("container-a"), labels); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "apps_lookup_resp_known_full.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewAppsLookupBuilder(buf, 1, 201)
		if err := b.Add(protocol.PidLookupKnown, protocol.AppsCgroupUnknownRetryLater,
			0, 1235, 1, 1000, 123457,
			[]byte("app"), []byte("/pending"), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "apps_lookup_resp_known_retry.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewAppsLookupBuilder(buf, 1, 202)
		if err := b.Add(protocol.PidLookupKnown, protocol.AppsCgroupUnknownPermanent,
			0, 1236, 1, 1000, 123458,
			[]byte("app2"), []byte("/permanent"), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "apps_lookup_resp_known_permanent.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewAppsLookupBuilder(buf, 1, 203)
		if err := b.Add(protocol.PidLookupKnown, protocol.AppsCgroupHostRoot,
			0, 1237, 1, 0, 123459,
			[]byte("sshd"), []byte(""), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "apps_lookup_resp_known_host_root.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewAppsLookupBuilder(buf, 1, 204)
		if err := b.Add(protocol.PidLookupUnknown, protocol.AppsCgroupKnown,
			0, 0, 0, protocol.NipcUIDUnset, 0,
			[]byte(""), []byte(""), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "apps_lookup_resp_unknown_pid.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewAppsLookupBuilder(buf, 1, 205)
		if err := b.Add(protocol.PidLookupPayloadExceeded, 0,
			0, 1238, 0, protocol.NipcUIDUnset, 0,
			[]byte(""), []byte(""), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "apps_lookup_resp_payload_exceeded.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewAppsLookupBuilder(buf, 1, 206)
		if err := b.Add(protocol.PidLookupOversizedItem, 0,
			0, 1239, 0, protocol.NipcUIDUnset, 0,
			[]byte(""), []byte(""), []byte(""), nil); err != nil {
			panic(err)
		}
		total := b.Finish()
		writeFile(dir, "apps_lookup_resp_oversized_item.bin", buf[:total])
	}
	{
		buf := make([]byte, 8192)
		b := protocol.NewAppsLookupBuilder(buf, 0, 207)
		total := b.Finish()
		writeFile(dir, "apps_lookup_resp_empty.bin", buf[:total])
	}
}

func doDecode(dir string) bool {
	c := &checker{}

	// 1. Outer message header.
	{
		data := readFile(dir, "header.bin")
		hdr, err := protocol.DecodeHeader(data)
		c.check(err == nil, "decode header")
		if err == nil {
			c.check(hdr.Magic == protocol.MagicMsg, "header magic")
			c.check(hdr.Version == protocol.Version, "header version")
			c.check(hdr.HeaderLen == protocol.HeaderLen, "header header_len")
			c.check(hdr.Kind == protocol.KindRequest, "header kind")
			c.check(hdr.Flags == protocol.FlagBatch, "header flags")
			c.check(hdr.Code == protocol.MethodCgroupsSnapshot, "header code")
			c.check(hdr.TransportStatus == protocol.StatusOK, "header transport_status")
			c.check(hdr.PayloadLen == 12345, "header payload_len")
			c.check(hdr.ItemCount == 42, "header item_count")
			c.check(hdr.MessageID == 0xDEADBEEFCAFEBABE, "header message_id")
		}
	}

	// 2. Chunk continuation header.
	{
		data := readFile(dir, "chunk_header.bin")
		chk, err := protocol.DecodeChunkHeader(data)
		c.check(err == nil, "decode chunk")
		if err == nil {
			c.check(chk.Magic == protocol.MagicChunk, "chunk magic")
			c.check(chk.MessageID == 0x1234567890ABCDEF, "chunk message_id")
			c.check(chk.TotalMessageLen == 100000, "chunk total_message_len")
			c.check(chk.ChunkIndex == 3, "chunk chunk_index")
			c.check(chk.ChunkCount == 10, "chunk chunk_count")
			c.check(chk.ChunkPayloadLen == 8192, "chunk chunk_payload_len")
		}
	}

	// 3. Hello payload.
	{
		data := readFile(dir, "hello.bin")
		h, err := protocol.DecodeHello(data)
		c.check(err == nil, "decode hello")
		if err == nil {
			c.check(h.SupportedProfiles == (protocol.ProfileBaseline|protocol.ProfileSHMFutex),
				"hello supported")
			c.check(h.PreferredProfiles == protocol.ProfileSHMFutex, "hello preferred")
			c.check(h.MaxRequestPayloadBytes == 4096, "hello max_req_payload")
			c.check(h.MaxRequestBatchItems == 100, "hello max_req_batch")
			c.check(h.MaxResponsePayloadBytes == 1048576, "hello max_resp_payload")
			c.check(h.MaxResponseBatchItems == 1, "hello max_resp_batch")
			c.check(h.AuthToken == 0xAABBCCDDEEFF0011, "hello auth_token")
			c.check(h.PacketSize == 65536, "hello packet_size")
		}
	}

	// 4. Hello-ack payload.
	{
		data := readFile(dir, "hello_ack.bin")
		h, err := protocol.DecodeHelloAck(data)
		c.check(err == nil, "decode hello_ack")
		if err == nil {
			c.check(h.ServerSupportedProfiles == 0x07, "hello_ack server_supported")
			c.check(h.IntersectionProfiles == 0x05, "hello_ack intersection")
			c.check(h.SelectedProfile == protocol.ProfileSHMFutex, "hello_ack selected")
			c.check(h.AgreedMaxRequestPayloadBytes == 2048, "hello_ack req_payload")
			c.check(h.AgreedMaxRequestBatchItems == 50, "hello_ack req_batch")
			c.check(h.AgreedMaxResponsePayloadBytes == 65536, "hello_ack resp_payload")
			c.check(h.AgreedMaxResponseBatchItems == 1, "hello_ack resp_batch")
			c.check(h.AgreedPacketSize == 32768, "hello_ack pkt_size")
			c.check(h.SessionID == 1, "hello_ack session_id")
		}
	}

	// 5. Cgroups request.
	{
		data := readFile(dir, "cgroups_req.bin")
		r, err := protocol.DecodeCgroupsRequest(data)
		c.check(err == nil, "decode cgroups_req")
		if err == nil {
			c.check(r.LayoutVersion == 1, "cgroups_req layout_version")
			c.check(r.Flags == 0, "cgroups_req flags")
		}
	}

	// 6. Cgroups snapshot response (multi-item).
	{
		data := readFile(dir, "cgroups_resp.bin")
		view, err := protocol.DecodeCgroupsResponse(data)
		c.check(err == nil, "decode cgroups_resp")
		if err == nil {
			c.check(view.ItemCount == 3, "cgroups_resp item_count")
			c.check(view.SystemdEnabled == 1, "cgroups_resp systemd_enabled")
			c.check(view.Generation == 999, "cgroups_resp generation")

			if item, err := view.Item(0); err == nil {
				c.check(item.Hash == 100, "item 0 hash")
				c.check(item.Options == 0, "item 0 options")
				c.check(item.Enabled == 1, "item 0 enabled")
				c.check(item.Name.String() == "init.scope", "item 0 name")
				c.check(item.Path.String() == "/sys/fs/cgroup/init.scope", "item 0 path")
			}

			if item, err := view.Item(1); err == nil {
				c.check(item.Hash == 200, "item 1 hash")
				c.check(item.Options == 0x02, "item 1 options")
				c.check(item.Enabled == 0, "item 1 enabled")
				c.check(item.Name.String() == "system.slice/docker-abc.scope",
					"item 1 name")
			}

			if item, err := view.Item(2); err == nil {
				c.check(item.Hash == 300, "item 2 hash")
				c.check(item.Name.Len() == 0, "item 2 name empty")
				c.check(item.Path.Len() == 0, "item 2 path empty")
			}
		}
	}

	// 7. Empty cgroups snapshot.
	{
		data := readFile(dir, "cgroups_resp_empty.bin")
		view, err := protocol.DecodeCgroupsResponse(data)
		c.check(err == nil, "decode cgroups_resp_empty")
		if err == nil {
			c.check(view.ItemCount == 0, "empty item_count")
			c.check(view.SystemdEnabled == 0, "empty systemd_enabled")
			c.check(view.Generation == 42, "empty generation")
		}
	}

	// 8. CGROUPS_LOOKUP request variants.
	{
		data := readFile(dir, "cgroups_lookup_req.bin")
		view, err := protocol.DecodeCgroupsLookupRequest(data)
		c.check(err == nil, "decode cgroups_lookup_req")
		if err == nil {
			c.check(view.ItemCount == 2, "cgroups_lookup_req item_count")
			item, _ := view.Item(0)
			c.check(item.String() == "/sys/fs/cgroup/a", "cgroups_lookup_req item0")
		}
	}
	{
		data := readFile(dir, "cgroups_lookup_req_empty.bin")
		view, err := protocol.DecodeCgroupsLookupRequest(data)
		c.check(err == nil, "decode cgroups_lookup_req_empty")
		if err == nil {
			c.check(view.ItemCount == 0, "cgroups_lookup_req_empty count")
		}
	}

	// 9. CGROUPS_LOOKUP response variants.
	{
		data := readFile(dir, "cgroups_lookup_resp_known_with_labels.bin")
		view, err := protocol.DecodeCgroupsLookupResponse(data)
		c.check(err == nil, "decode cgroups_lookup known labels")
		if err == nil {
			c.check(view.Generation == 100, "cgroups_lookup generation")
			item, _ := view.Item(0)
			c.check(item.Status == protocol.CgroupLookupKnown, "cgroups_lookup status")
			c.check(item.Orchestrator == protocol.OrchestratorK8s, "cgroups_lookup orchestrator")
			c.check(item.LabelCount == 2, "cgroups_lookup label_count")
			label, _ := item.Label(0)
			c.check(label.Key.String() == "namespace", "cgroups_lookup label")
		}
	}
	for _, file := range []string{
		"cgroups_lookup_resp_known_no_labels.bin",
		"cgroups_lookup_resp_unknown_retry.bin",
		"cgroups_lookup_resp_unknown_permanent.bin",
		"cgroups_lookup_resp_empty.bin",
	} {
		_, err := protocol.DecodeCgroupsLookupResponse(readFile(dir, file))
		c.check(err == nil, file)
	}
	{
		data := readFile(dir, "cgroups_lookup_resp_payload_exceeded.bin")
		view, err := protocol.DecodeCgroupsLookupResponse(data)
		c.check(err == nil, "decode cgroups_lookup payload_exceeded")
		if err == nil {
			item, _ := view.Item(0)
			c.check(item.Status == protocol.CgroupLookupPayloadExceeded, "cgroups_lookup payload_exceeded status")
			c.check(item.Path.String() == "/payload-exceeded", "cgroups_lookup payload_exceeded path")
			c.check(item.Name.Len() == 0, "cgroups_lookup payload_exceeded name")
		}
	}
	{
		data := readFile(dir, "cgroups_lookup_resp_oversized_item.bin")
		view, err := protocol.DecodeCgroupsLookupResponse(data)
		c.check(err == nil, "decode cgroups_lookup oversized_item")
		if err == nil {
			item, _ := view.Item(0)
			c.check(item.Status == protocol.CgroupLookupOversizedItem, "cgroups_lookup oversized_item status")
			c.check(item.Path.String() == "/oversized", "cgroups_lookup oversized_item path")
			c.check(item.Name.Len() == 0, "cgroups_lookup oversized_item name")
		}
	}

	// 10. APPS_LOOKUP request variants.
	{
		data := readFile(dir, "apps_lookup_req.bin")
		view, err := protocol.DecodeAppsLookupRequest(data)
		c.check(err == nil, "decode apps_lookup_req")
		if err == nil {
			c.check(view.ItemCount == 3, "apps_lookup_req item_count")
			pid, _ := view.Item(0)
			c.check(pid == 0, "apps_lookup_req pid0")
		}
	}
	{
		data := readFile(dir, "apps_lookup_req_empty.bin")
		view, err := protocol.DecodeAppsLookupRequest(data)
		c.check(err == nil, "decode apps_lookup_req_empty")
		if err == nil {
			c.check(view.ItemCount == 0, "apps_lookup_req_empty count")
		}
	}

	// 11. APPS_LOOKUP response variants.
	{
		data := readFile(dir, "apps_lookup_resp_known_full.bin")
		view, err := protocol.DecodeAppsLookupResponse(data)
		c.check(err == nil, "decode apps_lookup known full")
		if err == nil {
			item, _ := view.Item(0)
			c.check(item.Pid == 1234, "apps_lookup pid")
			c.check(item.Comm.Len() == 15, "apps_lookup comm boundary")
			c.check(item.CgroupStatus == protocol.AppsCgroupKnown, "apps_lookup cgroup status")
			label, _ := item.Label(0)
			c.check(label.Value.String() == "nginx:latest", "apps_lookup label")
		}
	}
	for _, file := range []string{
		"apps_lookup_resp_known_retry.bin",
		"apps_lookup_resp_known_permanent.bin",
		"apps_lookup_resp_known_host_root.bin",
		"apps_lookup_resp_unknown_pid.bin",
		"apps_lookup_resp_empty.bin",
	} {
		_, err := protocol.DecodeAppsLookupResponse(readFile(dir, file))
		c.check(err == nil, file)
	}
	{
		data := readFile(dir, "apps_lookup_resp_payload_exceeded.bin")
		view, err := protocol.DecodeAppsLookupResponse(data)
		c.check(err == nil, "decode apps_lookup payload_exceeded")
		if err == nil {
			item, _ := view.Item(0)
			c.check(item.Status == protocol.PidLookupPayloadExceeded, "apps_lookup payload_exceeded status")
			c.check(item.Pid == 1238, "apps_lookup payload_exceeded pid")
			c.check(item.Comm.Len() == 0, "apps_lookup payload_exceeded comm")
		}
	}
	{
		data := readFile(dir, "apps_lookup_resp_oversized_item.bin")
		view, err := protocol.DecodeAppsLookupResponse(data)
		c.check(err == nil, "decode apps_lookup oversized_item")
		if err == nil {
			item, _ := view.Item(0)
			c.check(item.Status == protocol.PidLookupOversizedItem, "apps_lookup oversized_item status")
			c.check(item.Pid == 1239, "apps_lookup oversized_item pid")
			c.check(item.Comm.Len() == 0, "apps_lookup oversized_item comm")
		}
	}

	return c.report("Go decode")
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "Usage: %s <encode|decode> <dir>\n", os.Args[0])
		os.Exit(1)
	}

	switch os.Args[1] {
	case "encode":
		doEncode(os.Args[2])
	case "decode":
		if !doDecode(os.Args[2]) {
			os.Exit(1)
		}
	default:
		fmt.Fprintf(os.Stderr, "Unknown command: %s\n", os.Args[1])
		os.Exit(1)
	}
}
