package main

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroupssnapshot"
)

func usage(argv0 string) {
	fmt.Fprintf(os.Stderr, "usage:\n")
	fmt.Fprintf(os.Stderr, "  %s encode-req <request_id> <value> <out_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s decode-req <in_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s encode-resp <request_id> <status> <value> <out_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s decode-resp <in_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s serve-once <req_file> <resp_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s server-once <service_namespace> <service> [auth_token]\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s server-loop <service_namespace> <service> <max_requests|0> [auth_token]\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s server-bench <service_namespace> <service> <max_requests|0> [auth_token]\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s encode-cgroups-req <message_id> <out_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s decode-cgroups-req <in_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s encode-cgroups-resp <message_id> <out_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s decode-cgroups-resp <in_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s serve-cgroups-once <req_file> <resp_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s client-refresh-once <service_namespace> <service> <lookup_hash> <lookup_name> [auth_token]\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s client-refresh-loop <service_namespace> <service> <iterations> <lookup_hash> <lookup_name> [auth_token]\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s client-refresh-bench <service_namespace> <service> <duration_sec> <target_rps> <lookup_hash> <lookup_name> [auth_token]\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s client-lookup-bench <service_namespace> <service> <duration_sec> <target_rps> <lookup_hash> <lookup_name> [auth_token]\n", argv0)
}

func mustParseU64(s string) uint64 {
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid u64 %q: %v\n", s, err)
		os.Exit(2)
	}
	return v
}

func mustParseI32(s string) int32 {
	v, err := strconv.ParseInt(s, 10, 32)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid i32 %q: %v\n", s, err)
		os.Exit(2)
	}
	return int32(v)
}

func mustParseU32(s string) uint32 {
	v := mustParseU64(s)
	if v > uint64(^uint32(0)) {
		fmt.Fprintf(os.Stderr, "invalid u32 %q: out of range\n", s)
		os.Exit(2)
	}
	return uint32(v)
}

func parseEnvU32(name string, fallback uint32) uint32 {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	return mustParseU32(value)
}

func applyCgroupsClientProfiles(config *cgroupssnapshot.Config) {
	config.SupportedProfiles = parseEnvU32("NETIPC_SUPPORTED_PROFILES", config.SupportedProfiles)
	config.PreferredProfiles = parseEnvU32("NETIPC_PREFERRED_PROFILES", config.PreferredProfiles)
}

func readBytes(path string) []byte {
	data, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "read failed for %s: %v\n", path, err)
		os.Exit(1)
	}
	return data
}

func writeBytes(path string, data []byte) {
	if err := os.WriteFile(path, data, 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "write failed for %s: %v\n", path, err)
		os.Exit(1)
	}
}

func readFrame(path string) protocol.Frame {
	data := readBytes(path)
	if len(data) != protocol.FrameSize {
		fmt.Fprintf(os.Stderr, "invalid frame size in %s: %d\n", path, len(data))
		os.Exit(1)
	}
	var frame protocol.Frame
	copy(frame[:], data)
	return frame
}

func writeFrame(path string, frame protocol.Frame) {
	writeBytes(path, frame[:])
}

func fixedCgroupsResponse(messageID uint64) []byte {
	payload := make([]byte, 1024)
	builder, err := protocol.NewCgroupsSnapshotResponseBuilder(payload, 42, true, 3, 2)
	if err != nil {
		fmt.Fprintf(os.Stderr, "snapshot builder init failed: %v\n", err)
		os.Exit(1)
	}
	if err := builder.AddItem(protocol.CgroupsSnapshotItem{
		Hash:    123,
		Options: 0x2,
		Enabled: true,
		Name:    "system.slice-nginx",
		Path:    "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs",
	}); err != nil {
		fmt.Fprintf(os.Stderr, "snapshot builder first item failed: %v\n", err)
		os.Exit(1)
	}
	if err := builder.AddItem(protocol.CgroupsSnapshotItem{
		Hash:    456,
		Options: 0x4,
		Enabled: false,
		Name:    "docker-1234",
		Path:    "",
	}); err != nil {
		fmt.Fprintf(os.Stderr, "snapshot builder second item failed: %v\n", err)
		os.Exit(1)
	}
	payloadLen, err := builder.Finish()
	if err != nil {
		fmt.Fprintf(os.Stderr, "snapshot builder finish failed: %v\n", err)
		os.Exit(1)
	}

	header, err := protocol.EncodeMessageHeader(protocol.MessageHeader{
		Magic:           protocol.MessageMagic,
		Version:         protocol.MessageVersion,
		HeaderLen:       protocol.MessageHeaderLen,
		Kind:            protocol.MessageKindResponse,
		Flags:           protocol.MessageFlagBatch,
		Code:            protocol.MethodCgroupsSnapshot,
		TransportStatus: protocol.TransportStatusOK,
		PayloadLen:      uint32(payloadLen),
		ItemCount:       2,
		MessageID:       messageID,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "encode cgroups response header failed: %v\n", err)
		os.Exit(1)
	}

	message := make([]byte, 0, protocol.MessageHeaderLen+payloadLen)
	message = append(message, header[:]...)
	message = append(message, payload[:payloadLen]...)
	return message
}

func boolToInt(v bool) int {
	if v {
		return 1
	}
	return 0
}

func printCgroupsCache(cache *cgroupssnapshot.Cache) {
	fmt.Printf("CGROUPS_CACHE\t%d\t%d\t%d\n", cache.Generation, boolToInt(cache.SystemdEnabled), len(cache.Items))
	for index, item := range cache.Items {
		fmt.Printf(
			"ITEM\t%d\t%d\t%d\t%d\t%s\t%s\n",
			index,
			item.Hash,
			item.Options,
			boolToInt(item.Enabled),
			item.Name,
			item.Path,
		)
	}
}

func clientRefreshOnce(serviceNamespace, serviceName string, lookupHash uint32, lookupName string, authToken uint64) {
	config := cgroupssnapshot.NewConfig(serviceNamespace, serviceName)
	applyCgroupsClientProfiles(&config)
	config.AuthToken = authToken
	client, err := cgroupssnapshot.NewClient(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client-refresh-once failed: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()
	if err := client.Refresh(10 * time.Second); err != nil {
		fmt.Fprintf(os.Stderr, "client-refresh-once failed: %v\n", err)
		os.Exit(1)
	}

	cache := client.Cache()
	printCgroupsCache(cache)
	item := client.Lookup(lookupHash, lookupName)
	if item != nil {
		fmt.Printf("LOOKUP\t%d\t%d\t%d\t%s\t%s\n", item.Hash, item.Options, boolToInt(item.Enabled), item.Name, item.Path)
		return
	}
	fmt.Printf("LOOKUP_MISS\t%d\t%s\n", lookupHash, lookupName)
}

func clientRefreshLoop(serviceNamespace, serviceName string, iterations uint64, lookupHash uint32, lookupName string, authToken uint64) {
	config := cgroupssnapshot.NewConfig(serviceNamespace, serviceName)
	applyCgroupsClientProfiles(&config)
	config.AuthToken = authToken
	client, err := cgroupssnapshot.NewClient(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client-refresh-loop failed: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()
	for i := uint64(0); i < iterations; i++ {
		if err := client.Refresh(10 * time.Second); err != nil {
			fmt.Fprintf(os.Stderr, "client-refresh-loop failed: %v\n", err)
			os.Exit(1)
		}
	}

	fmt.Printf("REFRESHES\t%d\n", iterations)
	cache := client.Cache()
	printCgroupsCache(cache)
	item := client.Lookup(lookupHash, lookupName)
	if item != nil {
		fmt.Printf("LOOKUP\t%d\t%d\t%d\t%s\t%s\n", item.Hash, item.Options, boolToInt(item.Enabled), item.Name, item.Path)
		return
	}
	fmt.Printf("LOOKUP_MISS\t%d\t%s\n", lookupHash, lookupName)
}

func main() {
	args := os.Args
	if len(args) < 2 {
		usage(args[0])
		os.Exit(2)
	}

	switch args[1] {
	case "encode-req":
		if len(args) != 5 {
			usage(args[0])
			os.Exit(2)
		}
		frame := protocol.EncodeIncrementRequest(
			mustParseU64(args[2]),
			protocol.IncrementRequest{Value: mustParseU64(args[3])},
		)
		writeFrame(args[4], frame)

	case "decode-req":
		if len(args) != 3 {
			usage(args[0])
			os.Exit(2)
		}
		requestID, request, err := protocol.DecodeIncrementRequest(readFrame(args[2]))
		if err != nil {
			fmt.Fprintf(os.Stderr, "decode-req failed: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("REQ %d %d\n", requestID, request.Value)

	case "encode-resp":
		if len(args) != 6 {
			usage(args[0])
			os.Exit(2)
		}
		frame := protocol.EncodeIncrementResponse(
			mustParseU64(args[2]),
			protocol.IncrementResponse{Status: mustParseI32(args[3]), Value: mustParseU64(args[4])},
		)
		writeFrame(args[5], frame)

	case "decode-resp":
		if len(args) != 3 {
			usage(args[0])
			os.Exit(2)
		}
		requestID, response, err := protocol.DecodeIncrementResponse(readFrame(args[2]))
		if err != nil {
			fmt.Fprintf(os.Stderr, "decode-resp failed: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("RESP %d %d %d\n", requestID, response.Status, response.Value)

	case "serve-once":
		if len(args) != 4 {
			usage(args[0])
			os.Exit(2)
		}
		requestID, request, err := protocol.DecodeIncrementRequest(readFrame(args[2]))
		if err != nil {
			fmt.Fprintf(os.Stderr, "serve-once decode failed: %v\n", err)
			os.Exit(1)
		}
		response := protocol.IncrementResponse{Status: protocol.StatusOK, Value: request.Value + 1}
		writeFrame(args[3], protocol.EncodeIncrementResponse(requestID, response))

	case "encode-cgroups-req":
		if len(args) != 4 {
			usage(args[0])
			os.Exit(2)
		}
		messageID := mustParseU64(args[2])
		payload := protocol.EncodeCgroupsSnapshotRequestPayload(protocol.CgroupsSnapshotRequest{Flags: 0})
		header, err := protocol.EncodeMessageHeader(protocol.MessageHeader{
			Magic:           protocol.MessageMagic,
			Version:         protocol.MessageVersion,
			HeaderLen:       protocol.MessageHeaderLen,
			Kind:            protocol.MessageKindRequest,
			Flags:           0,
			Code:            protocol.MethodCgroupsSnapshot,
			TransportStatus: protocol.TransportStatusOK,
			PayloadLen:      protocol.CgroupsSnapshotRequestPayloadLen,
			ItemCount:       1,
			MessageID:       messageID,
		})
		if err != nil {
			fmt.Fprintf(os.Stderr, "encode-cgroups-req failed: %v\n", err)
			os.Exit(1)
		}
		message := make([]byte, 0, protocol.MessageHeaderLen+len(payload))
		message = append(message, header[:]...)
		message = append(message, payload[:]...)
		writeBytes(args[3], message)

	case "decode-cgroups-req":
		if len(args) != 3 {
			usage(args[0])
			os.Exit(2)
		}
		message := readBytes(args[2])
		header, err := protocol.DecodeMessageHeader(message)
		if err != nil {
			fmt.Fprintf(os.Stderr, "decode-cgroups-req failed: %v\n", err)
			os.Exit(1)
		}
		if header.Kind != protocol.MessageKindRequest ||
			header.Code != protocol.MethodCgroupsSnapshot ||
			header.TransportStatus != protocol.TransportStatusOK ||
			header.Flags != 0 ||
			header.ItemCount != 1 ||
			header.PayloadLen != protocol.CgroupsSnapshotRequestPayloadLen {
			fmt.Fprintf(os.Stderr, "decode-cgroups-req failed: invalid header\n")
			os.Exit(1)
		}
		view, err := protocol.DecodeCgroupsSnapshotRequestView(message[protocol.MessageHeaderLen:])
		if err != nil {
			fmt.Fprintf(os.Stderr, "decode-cgroups-req failed: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("CGROUPS_REQ\t%d\t%d\n", header.MessageID, view.Flags)

	case "encode-cgroups-resp":
		if len(args) != 4 {
			usage(args[0])
			os.Exit(2)
		}
		writeBytes(args[3], fixedCgroupsResponse(mustParseU64(args[2])))

	case "decode-cgroups-resp":
		if len(args) != 3 {
			usage(args[0])
			os.Exit(2)
		}
		message := readBytes(args[2])
		header, err := protocol.DecodeMessageHeader(message)
		if err != nil {
			fmt.Fprintf(os.Stderr, "decode-cgroups-resp failed: %v\n", err)
			os.Exit(1)
		}
		if header.Kind != protocol.MessageKindResponse ||
			header.Code != protocol.MethodCgroupsSnapshot ||
			header.TransportStatus != protocol.TransportStatusOK ||
			header.Flags != protocol.MessageFlagBatch {
			fmt.Fprintf(os.Stderr, "decode-cgroups-resp failed: invalid header\n")
			os.Exit(1)
		}
		view, err := protocol.DecodeCgroupsSnapshotView(message[protocol.MessageHeaderLen:], header.ItemCount)
		if err != nil {
			fmt.Fprintf(os.Stderr, "decode-cgroups-resp failed: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("CGROUPS_RESP\t%d\t%d\t%d\t%d\n", header.MessageID, view.Generation, boolToInt(view.SystemdEnabled), view.ItemCount)
		for index := uint32(0); index < view.ItemCount; index++ {
			item, err := view.ItemViewAt(index)
			if err != nil {
				fmt.Fprintf(os.Stderr, "decode-cgroups-resp failed: %v\n", err)
				os.Exit(1)
			}
			fmt.Printf(
				"ITEM\t%d\t%d\t%d\t%d\t%s\t%s\n",
				index,
				item.Hash,
				item.Options,
				boolToInt(item.Enabled),
				item.NameView.CopyString(),
				item.PathView.CopyString(),
			)
		}

	case "serve-cgroups-once":
		if len(args) != 4 {
			usage(args[0])
			os.Exit(2)
		}
		message := readBytes(args[2])
		header, err := protocol.DecodeMessageHeader(message)
		if err != nil {
			fmt.Fprintf(os.Stderr, "serve-cgroups-once decode failed: %v\n", err)
			os.Exit(1)
		}
		if header.Kind != protocol.MessageKindRequest ||
			header.Code != protocol.MethodCgroupsSnapshot ||
			header.TransportStatus != protocol.TransportStatusOK ||
			header.Flags != 0 ||
			header.ItemCount != 1 ||
			header.PayloadLen != protocol.CgroupsSnapshotRequestPayloadLen {
			fmt.Fprintf(os.Stderr, "serve-cgroups-once decode failed: invalid header\n")
			os.Exit(1)
		}
		if _, err := protocol.DecodeCgroupsSnapshotRequestView(message[protocol.MessageHeaderLen:]); err != nil {
			fmt.Fprintf(os.Stderr, "serve-cgroups-once decode failed: %v\n", err)
			os.Exit(1)
		}
		writeBytes(args[3], fixedCgroupsResponse(header.MessageID))

	case "server-once":
		if len(args) != 4 && len(args) != 5 {
			usage(args[0])
			os.Exit(2)
		}
		authToken := uint64(0)
		if len(args) == 5 {
			authToken = mustParseU64(args[4])
		}
		runCgroupsServerOnce(args[2], args[3], authToken)

	case "server-loop":
		if len(args) != 5 && len(args) != 6 {
			usage(args[0])
			os.Exit(2)
		}
		authToken := uint64(0)
		if len(args) == 6 {
			authToken = mustParseU64(args[5])
		}
		runCgroupsServerLoop(args[2], args[3], mustParseU64(args[4]), authToken)

	case "server-bench":
		if len(args) != 5 && len(args) != 6 {
			usage(args[0])
			os.Exit(2)
		}
		authToken := uint64(0)
		if len(args) == 6 {
			authToken = mustParseU64(args[5])
		}
		runCgroupsServerBench(args[2], args[3], mustParseU64(args[4]), authToken)

	case "client-refresh-once":
		if len(args) != 6 && len(args) != 7 {
			usage(args[0])
			os.Exit(2)
		}
		authToken := uint64(0)
		if len(args) == 7 {
			authToken = mustParseU64(args[6])
		}
		clientRefreshOnce(args[2], args[3], mustParseU32(args[4]), args[5], authToken)

	case "client-refresh-loop":
		if len(args) != 7 && len(args) != 8 {
			usage(args[0])
			os.Exit(2)
		}
		authToken := uint64(0)
		if len(args) == 8 {
			authToken = mustParseU64(args[7])
		}
		clientRefreshLoop(args[2], args[3], mustParseU64(args[4]), mustParseU32(args[5]), args[6], authToken)

	case "client-refresh-bench":
		if len(args) != 8 && len(args) != 9 {
			usage(args[0])
			os.Exit(2)
		}
		authToken := uint64(0)
		if len(args) == 9 {
			authToken = mustParseU64(args[8])
		}
		clientRefreshBench(
			args[2],
			args[3],
			int(mustParseU64(args[4])),
			int(mustParseU64(args[5])),
			mustParseU32(args[6]),
			args[7],
			authToken,
		)

	case "client-lookup-bench":
		if len(args) != 8 && len(args) != 9 {
			usage(args[0])
			os.Exit(2)
		}
		authToken := uint64(0)
		if len(args) == 9 {
			authToken = mustParseU64(args[8])
		}
		clientLookupBench(
			args[2],
			args[3],
			int(mustParseU64(args[4])),
			int(mustParseU64(args[5])),
			mustParseU32(args[6]),
			args[7],
			authToken,
		)

	default:
		usage(args[0])
		os.Exit(2)
	}
}
