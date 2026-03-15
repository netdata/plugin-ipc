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
	if err := os.WriteFile(path, data, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "ERROR: cannot write %s: %v\n", path, err)
		os.Exit(1)
	}
}

func readFile(dir, name string) []byte {
	path := filepath.Join(dir, name)
	data, err := os.ReadFile(path)
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

		b.Add(100, 0, 1,
			[]byte("init.scope"),
			[]byte("/sys/fs/cgroup/init.scope"))
		b.Add(200, 0x02, 0,
			[]byte("system.slice/docker-abc.scope"),
			[]byte("/sys/fs/cgroup/system.slice/docker-abc.scope"))
		b.Add(300, 0, 1, []byte(""), []byte(""))

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
