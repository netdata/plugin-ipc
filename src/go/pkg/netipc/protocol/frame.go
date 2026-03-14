package protocol

import (
	"encoding/binary"
	"fmt"
)

const (
	MessageMagic         uint32 = 0x4e495043
	MessageVersion       uint16 = 1
	MessageHeaderLen            = 32
	MessageItemRefLen           = 8
	MessageItemAlignment        = 8
	MessageFlagBatch     uint16 = 0x0001

	MessageKindRequest  uint16 = 1
	MessageKindResponse uint16 = 2
	MessageKindControl  uint16 = 3

	ControlHello              uint16 = 1
	ControlHelloAck           uint16 = 2
	ControlHelloPayloadLen           = 44
	ControlHelloAckPayloadLen        = 36
	ChunkMagic                uint32 = 0x4e43484b
	ChunkVersion              uint16 = 1
	ChunkHeaderLen                   = 32

	TransportStatusOK            uint16 = 0
	TransportStatusBadEnvelope   uint16 = 1
	TransportStatusAuthFailed    uint16 = 2
	TransportStatusIncompatible  uint16 = 3
	TransportStatusUnsupported   uint16 = 4
	TransportStatusLimitExceeded uint16 = 5
	TransportStatusInternalError uint16 = 6

	MaxPayloadDefault uint32 = 1024

	// Legacy fixed-frame increment API stays exported while transports migrate.
	FrameMagic   uint32 = MessageMagic
	FrameVersion uint16 = MessageVersion
	FrameSize           = 64

	FrameKindRequest  uint16 = MessageKindRequest
	FrameKindResponse uint16 = MessageKindResponse

	MethodIncrement       uint16 = 1
	MethodCgroupsSnapshot uint16 = 2
	IncrementPayloadLen   uint32 = 12

	CgroupsSnapshotLayoutVersion     uint16 = 1
	CgroupsSnapshotRequestPayloadLen        = 4
	CgroupsSnapshotResponseHeaderLen        = 16
	CgroupsSnapshotItemHeaderLen            = 32

	StatusOK            int32 = 0
	StatusBadRequest    int32 = 1
	StatusInternalError int32 = 2
)

const (
	offMagic           = 0
	offVersion         = 4
	offHeaderLen       = 6
	offKind            = 8
	offFlags           = 10
	offCode            = 12
	offTransportStatus = 14
	offPayloadLen      = 16
	offItemCount       = 20
	offMessageID       = 24

	offItemRefOffset = 0
	offItemRefLength = 4

	offHelloLayoutVersion         = 0
	offHelloFlags                 = 2
	offHelloSupported             = 4
	offHelloPreferred             = 8
	offHelloMaxRequestPayload     = 12
	offHelloMaxRequestBatchItems  = 16
	offHelloMaxResponsePayload    = 20
	offHelloMaxResponseBatchItems = 24
	offHelloAuthToken             = 32
	offHelloPacketSize            = 40

	offHelloAckLayoutVersion            = 0
	offHelloAckFlags                    = 2
	offHelloAckServerSupported          = 4
	offHelloAckIntersection             = 8
	offHelloAckSelected                 = 12
	offHelloAckAgreedMaxRequestPayload  = 16
	offHelloAckAgreedMaxRequestItems    = 20
	offHelloAckAgreedMaxResponsePayload = 24
	offHelloAckAgreedMaxResponseItems   = 28
	offHelloAckAgreedPacketSize         = 32

	offChunkMagic           = 0
	offChunkVersion         = 4
	offChunkFlags           = 6
	offChunkMessageID       = 8
	offChunkTotalMessageLen = 16
	offChunkIndex           = 20
	offChunkCount           = 24
	offChunkPayloadLen      = 28

	offCgroupsSnapshotReqLayoutVersion = 0
	offCgroupsSnapshotReqFlags         = 2

	offCgroupsSnapshotRespLayoutVersion = 0
	offCgroupsSnapshotRespFlags         = 2
	offCgroupsSnapshotRespSystemd       = 4
	offCgroupsSnapshotRespGeneration    = 8

	offCgroupsSnapshotItemLayoutVersion = 0
	offCgroupsSnapshotItemFlags         = 2
	offCgroupsSnapshotItemHash          = 4
	offCgroupsSnapshotItemOptions       = 8
	offCgroupsSnapshotItemEnabled       = 12
	offCgroupsSnapshotItemNameOff       = 16
	offCgroupsSnapshotItemNameLen       = 20
	offCgroupsSnapshotItemPathOff       = 24
	offCgroupsSnapshotItemPathLen       = 28

	offValue  = 32
	offStatus = 40
)

type Frame [FrameSize]byte

type MessageHeader struct {
	Magic           uint32
	Version         uint16
	HeaderLen       uint16
	Kind            uint16
	Flags           uint16
	Code            uint16
	TransportStatus uint16
	PayloadLen      uint32
	ItemCount       uint32
	MessageID       uint64
}

type ItemRef struct {
	Offset uint32
	Length uint32
}

type HelloPayload struct {
	LayoutVersion           uint16
	Flags                   uint16
	Supported               uint32
	Preferred               uint32
	MaxRequestPayloadBytes  uint32
	MaxRequestBatchItems    uint32
	MaxResponsePayloadBytes uint32
	MaxResponseBatchItems   uint32
	AuthToken               uint64
	PacketSize              uint32
}

type HelloAckPayload struct {
	LayoutVersion               uint16
	Flags                       uint16
	ServerSupported             uint32
	Intersection                uint32
	Selected                    uint32
	AgreedMaxRequestPayload     uint32
	AgreedMaxRequestBatchItems  uint32
	AgreedMaxResponsePayload    uint32
	AgreedMaxResponseBatchItems uint32
	AgreedPacketSize            uint32
}

type ChunkHeader struct {
	Magic           uint32
	Version         uint16
	Flags           uint16
	MessageID       uint64
	TotalMessageLen uint32
	ChunkIndex      uint32
	ChunkCount      uint32
	ChunkPayloadLen uint32
}

type IncrementRequest struct {
	Value uint64
}

type IncrementResponse struct {
	Status int32
	Value  uint64
}

type CgroupsSnapshotRequest struct {
	Flags uint16
}

type CgroupsSnapshotRequestView struct {
	LayoutVersion uint16
	Flags         uint16
}

type CStringView struct {
	bytes []byte
}

func (v CStringView) Len() int {
	return len(v.bytes)
}

func (v CStringView) IsEmpty() bool {
	return len(v.bytes) == 0
}

func (v CStringView) CopyString() string {
	return string(v.bytes)
}

type CgroupsSnapshotItem struct {
	Hash    uint32
	Options uint32
	Enabled bool
	Name    string
	Path    string
}

type CgroupsSnapshotItemView struct {
	LayoutVersion uint16
	Flags         uint16
	Hash          uint32
	Options       uint32
	Enabled       bool
	NameView      CStringView
	PathView      CStringView
}

type CgroupsSnapshotView struct {
	LayoutVersion  uint16
	Flags          uint16
	Generation     uint64
	SystemdEnabled bool
	ItemCount      uint32
	itemRefs       []byte
	items          []byte
}

type CgroupsSnapshotResponseBuilder struct {
	buf            []byte
	payloadLen     int
	packedOffset   int
	expectedItems  uint32
	itemCount      uint32
	flags          uint16
	systemdEnabled bool
	generation     uint64
}

func validateMessageHeader(header MessageHeader) error {
	if header.Magic != MessageMagic || header.Version != MessageVersion {
		return fmt.Errorf("invalid message magic/version")
	}
	if header.HeaderLen != MessageHeaderLen {
		return fmt.Errorf("invalid message header_len")
	}
	switch header.Kind {
	case MessageKindRequest, MessageKindResponse, MessageKindControl:
		return nil
	default:
		return fmt.Errorf("invalid message kind")
	}
}

func MessageTotalSize(header MessageHeader) (int, error) {
	if err := validateMessageHeader(header); err != nil {
		return 0, err
	}
	return MessageHeaderLen + int(header.PayloadLen), nil
}

func AlignedItemSize(payloadLen uint32) (int, error) {
	aligned := int(payloadLen)
	rem := aligned % MessageItemAlignment
	if rem == 0 {
		return aligned, nil
	}
	return aligned + (MessageItemAlignment - rem), nil
}

func MaxBatchPayloadLen(maxItemPayloadLen uint32, maxItems uint32) (int, error) {
	if maxItemPayloadLen == 0 || maxItems == 0 {
		return 0, fmt.Errorf("invalid negotiated max payload/items")
	}

	itemCount := int(maxItems)
	tableLen := itemCount * MessageItemRefLen
	if itemCount != 0 && tableLen/MessageItemRefLen != itemCount {
		return 0, fmt.Errorf("item-ref table size overflow")
	}

	alignedItemLen, err := AlignedItemSize(maxItemPayloadLen)
	if err != nil {
		return 0, err
	}

	payloadAreaLen := itemCount * alignedItemLen
	if itemCount != 0 && payloadAreaLen/alignedItemLen != itemCount {
		return 0, fmt.Errorf("payload area size overflow")
	}
	if tableLen > int(^uint(0)>>1)-payloadAreaLen {
		return 0, fmt.Errorf("batch payload size overflow")
	}

	return tableLen + payloadAreaLen, nil
}

func MaxBatchTotalSize(maxItemPayloadLen uint32, maxItems uint32) (int, error) {
	payloadLen, err := MaxBatchPayloadLen(maxItemPayloadLen, maxItems)
	if err != nil {
		return 0, err
	}
	if MessageHeaderLen > int(^uint(0)>>1)-payloadLen {
		return 0, fmt.Errorf("batch total size overflow")
	}
	return MessageHeaderLen + payloadLen, nil
}

func EncodeMessageHeader(header MessageHeader) ([MessageHeaderLen]byte, error) {
	var buf [MessageHeaderLen]byte
	if err := validateMessageHeader(header); err != nil {
		return buf, err
	}

	binary.LittleEndian.PutUint32(buf[offMagic:offMagic+4], header.Magic)
	binary.LittleEndian.PutUint16(buf[offVersion:offVersion+2], header.Version)
	binary.LittleEndian.PutUint16(buf[offHeaderLen:offHeaderLen+2], header.HeaderLen)
	binary.LittleEndian.PutUint16(buf[offKind:offKind+2], header.Kind)
	binary.LittleEndian.PutUint16(buf[offFlags:offFlags+2], header.Flags)
	binary.LittleEndian.PutUint16(buf[offCode:offCode+2], header.Code)
	binary.LittleEndian.PutUint16(buf[offTransportStatus:offTransportStatus+2], header.TransportStatus)
	binary.LittleEndian.PutUint32(buf[offPayloadLen:offPayloadLen+4], header.PayloadLen)
	binary.LittleEndian.PutUint32(buf[offItemCount:offItemCount+4], header.ItemCount)
	binary.LittleEndian.PutUint64(buf[offMessageID:offMessageID+8], header.MessageID)
	return buf, nil
}

func DecodeMessageHeader(buf []byte) (MessageHeader, error) {
	if len(buf) < MessageHeaderLen {
		return MessageHeader{}, fmt.Errorf("short message header")
	}

	header := MessageHeader{
		Magic:           binary.LittleEndian.Uint32(buf[offMagic : offMagic+4]),
		Version:         binary.LittleEndian.Uint16(buf[offVersion : offVersion+2]),
		HeaderLen:       binary.LittleEndian.Uint16(buf[offHeaderLen : offHeaderLen+2]),
		Kind:            binary.LittleEndian.Uint16(buf[offKind : offKind+2]),
		Flags:           binary.LittleEndian.Uint16(buf[offFlags : offFlags+2]),
		Code:            binary.LittleEndian.Uint16(buf[offCode : offCode+2]),
		TransportStatus: binary.LittleEndian.Uint16(buf[offTransportStatus : offTransportStatus+2]),
		PayloadLen:      binary.LittleEndian.Uint32(buf[offPayloadLen : offPayloadLen+4]),
		ItemCount:       binary.LittleEndian.Uint32(buf[offItemCount : offItemCount+4]),
		MessageID:       binary.LittleEndian.Uint64(buf[offMessageID : offMessageID+8]),
	}
	if err := validateMessageHeader(header); err != nil {
		return MessageHeader{}, err
	}
	return header, nil
}

func EncodeItemRefs(refs []ItemRef) []byte {
	buf := make([]byte, len(refs)*MessageItemRefLen)
	for i, ref := range refs {
		base := i * MessageItemRefLen
		binary.LittleEndian.PutUint32(buf[base+offItemRefOffset:base+offItemRefOffset+4], ref.Offset)
		binary.LittleEndian.PutUint32(buf[base+offItemRefLength:base+offItemRefLength+4], ref.Length)
	}
	return buf
}

func DecodeItemRefs(buf []byte, itemCount int) ([]ItemRef, error) {
	if len(buf) < itemCount*MessageItemRefLen {
		return nil, fmt.Errorf("short item-ref table")
	}
	refs := make([]ItemRef, itemCount)
	for i := 0; i < itemCount; i++ {
		base := i * MessageItemRefLen
		refs[i] = ItemRef{
			Offset: binary.LittleEndian.Uint32(buf[base+offItemRefOffset : base+offItemRefOffset+4]),
			Length: binary.LittleEndian.Uint32(buf[base+offItemRefLength : base+offItemRefLength+4]),
		}
	}
	return refs, nil
}

func EncodeHelloPayload(payload HelloPayload) [ControlHelloPayloadLen]byte {
	var buf [ControlHelloPayloadLen]byte
	binary.LittleEndian.PutUint16(buf[offHelloLayoutVersion:offHelloLayoutVersion+2], payload.LayoutVersion)
	binary.LittleEndian.PutUint16(buf[offHelloFlags:offHelloFlags+2], payload.Flags)
	binary.LittleEndian.PutUint32(buf[offHelloSupported:offHelloSupported+4], payload.Supported)
	binary.LittleEndian.PutUint32(buf[offHelloPreferred:offHelloPreferred+4], payload.Preferred)
	binary.LittleEndian.PutUint32(buf[offHelloMaxRequestPayload:offHelloMaxRequestPayload+4], payload.MaxRequestPayloadBytes)
	binary.LittleEndian.PutUint32(buf[offHelloMaxRequestBatchItems:offHelloMaxRequestBatchItems+4], payload.MaxRequestBatchItems)
	binary.LittleEndian.PutUint32(buf[offHelloMaxResponsePayload:offHelloMaxResponsePayload+4], payload.MaxResponsePayloadBytes)
	binary.LittleEndian.PutUint32(buf[offHelloMaxResponseBatchItems:offHelloMaxResponseBatchItems+4], payload.MaxResponseBatchItems)
	binary.LittleEndian.PutUint64(buf[offHelloAuthToken:offHelloAuthToken+8], payload.AuthToken)
	binary.LittleEndian.PutUint32(buf[offHelloPacketSize:offHelloPacketSize+4], payload.PacketSize)
	return buf
}

func DecodeHelloPayload(buf []byte) (HelloPayload, error) {
	if len(buf) < ControlHelloPayloadLen {
		return HelloPayload{}, fmt.Errorf("short hello payload")
	}
	return HelloPayload{
		LayoutVersion:           binary.LittleEndian.Uint16(buf[offHelloLayoutVersion : offHelloLayoutVersion+2]),
		Flags:                   binary.LittleEndian.Uint16(buf[offHelloFlags : offHelloFlags+2]),
		Supported:               binary.LittleEndian.Uint32(buf[offHelloSupported : offHelloSupported+4]),
		Preferred:               binary.LittleEndian.Uint32(buf[offHelloPreferred : offHelloPreferred+4]),
		MaxRequestPayloadBytes:  binary.LittleEndian.Uint32(buf[offHelloMaxRequestPayload : offHelloMaxRequestPayload+4]),
		MaxRequestBatchItems:    binary.LittleEndian.Uint32(buf[offHelloMaxRequestBatchItems : offHelloMaxRequestBatchItems+4]),
		MaxResponsePayloadBytes: binary.LittleEndian.Uint32(buf[offHelloMaxResponsePayload : offHelloMaxResponsePayload+4]),
		MaxResponseBatchItems:   binary.LittleEndian.Uint32(buf[offHelloMaxResponseBatchItems : offHelloMaxResponseBatchItems+4]),
		AuthToken:               binary.LittleEndian.Uint64(buf[offHelloAuthToken : offHelloAuthToken+8]),
		PacketSize:              binary.LittleEndian.Uint32(buf[offHelloPacketSize : offHelloPacketSize+4]),
	}, nil
}

func EncodeHelloAckPayload(payload HelloAckPayload) [ControlHelloAckPayloadLen]byte {
	var buf [ControlHelloAckPayloadLen]byte
	binary.LittleEndian.PutUint16(buf[offHelloAckLayoutVersion:offHelloAckLayoutVersion+2], payload.LayoutVersion)
	binary.LittleEndian.PutUint16(buf[offHelloAckFlags:offHelloAckFlags+2], payload.Flags)
	binary.LittleEndian.PutUint32(buf[offHelloAckServerSupported:offHelloAckServerSupported+4], payload.ServerSupported)
	binary.LittleEndian.PutUint32(buf[offHelloAckIntersection:offHelloAckIntersection+4], payload.Intersection)
	binary.LittleEndian.PutUint32(buf[offHelloAckSelected:offHelloAckSelected+4], payload.Selected)
	binary.LittleEndian.PutUint32(buf[offHelloAckAgreedMaxRequestPayload:offHelloAckAgreedMaxRequestPayload+4], payload.AgreedMaxRequestPayload)
	binary.LittleEndian.PutUint32(buf[offHelloAckAgreedMaxRequestItems:offHelloAckAgreedMaxRequestItems+4], payload.AgreedMaxRequestBatchItems)
	binary.LittleEndian.PutUint32(buf[offHelloAckAgreedMaxResponsePayload:offHelloAckAgreedMaxResponsePayload+4], payload.AgreedMaxResponsePayload)
	binary.LittleEndian.PutUint32(buf[offHelloAckAgreedMaxResponseItems:offHelloAckAgreedMaxResponseItems+4], payload.AgreedMaxResponseBatchItems)
	binary.LittleEndian.PutUint32(buf[offHelloAckAgreedPacketSize:offHelloAckAgreedPacketSize+4], payload.AgreedPacketSize)
	return buf
}

func DecodeHelloAckPayload(buf []byte) (HelloAckPayload, error) {
	if len(buf) < ControlHelloAckPayloadLen {
		return HelloAckPayload{}, fmt.Errorf("short hello-ack payload")
	}
	return HelloAckPayload{
		LayoutVersion:               binary.LittleEndian.Uint16(buf[offHelloAckLayoutVersion : offHelloAckLayoutVersion+2]),
		Flags:                       binary.LittleEndian.Uint16(buf[offHelloAckFlags : offHelloAckFlags+2]),
		ServerSupported:             binary.LittleEndian.Uint32(buf[offHelloAckServerSupported : offHelloAckServerSupported+4]),
		Intersection:                binary.LittleEndian.Uint32(buf[offHelloAckIntersection : offHelloAckIntersection+4]),
		Selected:                    binary.LittleEndian.Uint32(buf[offHelloAckSelected : offHelloAckSelected+4]),
		AgreedMaxRequestPayload:     binary.LittleEndian.Uint32(buf[offHelloAckAgreedMaxRequestPayload : offHelloAckAgreedMaxRequestPayload+4]),
		AgreedMaxRequestBatchItems:  binary.LittleEndian.Uint32(buf[offHelloAckAgreedMaxRequestItems : offHelloAckAgreedMaxRequestItems+4]),
		AgreedMaxResponsePayload:    binary.LittleEndian.Uint32(buf[offHelloAckAgreedMaxResponsePayload : offHelloAckAgreedMaxResponsePayload+4]),
		AgreedMaxResponseBatchItems: binary.LittleEndian.Uint32(buf[offHelloAckAgreedMaxResponseItems : offHelloAckAgreedMaxResponseItems+4]),
		AgreedPacketSize:            binary.LittleEndian.Uint32(buf[offHelloAckAgreedPacketSize : offHelloAckAgreedPacketSize+4]),
	}, nil
}

func EncodeChunkHeader(header ChunkHeader) ([ChunkHeaderLen]byte, error) {
	var buf [ChunkHeaderLen]byte
	if header.Magic != ChunkMagic || header.Version != ChunkVersion ||
		header.ChunkCount == 0 || header.ChunkIndex >= header.ChunkCount ||
		header.ChunkPayloadLen == 0 || header.TotalMessageLen == 0 {
		return buf, fmt.Errorf("invalid chunk header")
	}
	binary.LittleEndian.PutUint32(buf[offChunkMagic:offChunkMagic+4], header.Magic)
	binary.LittleEndian.PutUint16(buf[offChunkVersion:offChunkVersion+2], header.Version)
	binary.LittleEndian.PutUint16(buf[offChunkFlags:offChunkFlags+2], header.Flags)
	binary.LittleEndian.PutUint64(buf[offChunkMessageID:offChunkMessageID+8], header.MessageID)
	binary.LittleEndian.PutUint32(buf[offChunkTotalMessageLen:offChunkTotalMessageLen+4], header.TotalMessageLen)
	binary.LittleEndian.PutUint32(buf[offChunkIndex:offChunkIndex+4], header.ChunkIndex)
	binary.LittleEndian.PutUint32(buf[offChunkCount:offChunkCount+4], header.ChunkCount)
	binary.LittleEndian.PutUint32(buf[offChunkPayloadLen:offChunkPayloadLen+4], header.ChunkPayloadLen)
	return buf, nil
}

func DecodeChunkHeader(buf []byte) (ChunkHeader, error) {
	if len(buf) < ChunkHeaderLen {
		return ChunkHeader{}, fmt.Errorf("short chunk header")
	}
	header := ChunkHeader{
		Magic:           binary.LittleEndian.Uint32(buf[offChunkMagic : offChunkMagic+4]),
		Version:         binary.LittleEndian.Uint16(buf[offChunkVersion : offChunkVersion+2]),
		Flags:           binary.LittleEndian.Uint16(buf[offChunkFlags : offChunkFlags+2]),
		MessageID:       binary.LittleEndian.Uint64(buf[offChunkMessageID : offChunkMessageID+8]),
		TotalMessageLen: binary.LittleEndian.Uint32(buf[offChunkTotalMessageLen : offChunkTotalMessageLen+4]),
		ChunkIndex:      binary.LittleEndian.Uint32(buf[offChunkIndex : offChunkIndex+4]),
		ChunkCount:      binary.LittleEndian.Uint32(buf[offChunkCount : offChunkCount+4]),
		ChunkPayloadLen: binary.LittleEndian.Uint32(buf[offChunkPayloadLen : offChunkPayloadLen+4]),
	}
	if header.Magic != ChunkMagic || header.Version != ChunkVersion ||
		header.ChunkCount == 0 || header.ChunkIndex >= header.ChunkCount ||
		header.ChunkPayloadLen == 0 || header.TotalMessageLen == 0 {
		return ChunkHeader{}, fmt.Errorf("invalid chunk header")
	}
	return header, nil
}

func CgroupsSnapshotItemPayloadLen(item CgroupsSnapshotItem) (int, error) {
	total := CgroupsSnapshotItemHeaderLen
	if total > int(^uint(0)>>1)-len(item.Name)-1 {
		return 0, fmt.Errorf("cgroups snapshot item size overflow")
	}
	total += len(item.Name) + 1
	if total > int(^uint(0)>>1)-len(item.Path)-1 {
		return 0, fmt.Errorf("cgroups snapshot item size overflow")
	}
	total += len(item.Path) + 1
	return total, nil
}

func EncodeCgroupsSnapshotRequestPayload(request CgroupsSnapshotRequest) [CgroupsSnapshotRequestPayloadLen]byte {
	var buf [CgroupsSnapshotRequestPayloadLen]byte
	binary.LittleEndian.PutUint16(buf[offCgroupsSnapshotReqLayoutVersion:offCgroupsSnapshotReqLayoutVersion+2], CgroupsSnapshotLayoutVersion)
	binary.LittleEndian.PutUint16(buf[offCgroupsSnapshotReqFlags:offCgroupsSnapshotReqFlags+2], request.Flags)
	return buf
}

func DecodeCgroupsSnapshotRequestView(buf []byte) (CgroupsSnapshotRequestView, error) {
	if len(buf) < CgroupsSnapshotRequestPayloadLen {
		return CgroupsSnapshotRequestView{}, fmt.Errorf("short cgroups snapshot request payload")
	}
	view := CgroupsSnapshotRequestView{
		LayoutVersion: binary.LittleEndian.Uint16(buf[offCgroupsSnapshotReqLayoutVersion : offCgroupsSnapshotReqLayoutVersion+2]),
		Flags:         binary.LittleEndian.Uint16(buf[offCgroupsSnapshotReqFlags : offCgroupsSnapshotReqFlags+2]),
	}
	if view.LayoutVersion != CgroupsSnapshotLayoutVersion {
		return CgroupsSnapshotRequestView{}, fmt.Errorf("invalid cgroups snapshot request layout version")
	}
	return view, nil
}

func validateCStringView(payload []byte, off uint32, length uint32) (CStringView, error) {
	start := int(off)
	size := int(length)
	if start < CgroupsSnapshotItemHeaderLen {
		return CStringView{}, fmt.Errorf("cgroups snapshot string overlaps item header")
	}
	if start < 0 || size < 0 {
		return CStringView{}, fmt.Errorf("invalid cgroups snapshot string bounds")
	}
	end := start + size + 1
	if end < start || end > len(payload) {
		return CStringView{}, fmt.Errorf("cgroups snapshot string out of bounds")
	}
	if payload[start+size] != 0 {
		return CStringView{}, fmt.Errorf("cgroups snapshot string is not NUL terminated")
	}
	return CStringView{bytes: payload[start : start+size]}, nil
}

func DecodeCgroupsSnapshotView(payload []byte, itemCount uint32) (CgroupsSnapshotView, error) {
	refsLen := int(itemCount) * MessageItemRefLen
	if itemCount != 0 && refsLen/MessageItemRefLen != int(itemCount) {
		return CgroupsSnapshotView{}, fmt.Errorf("cgroups snapshot item-ref table overflow")
	}
	packedOffset := CgroupsSnapshotResponseHeaderLen + refsLen
	if packedOffset < CgroupsSnapshotResponseHeaderLen || len(payload) < packedOffset {
		return CgroupsSnapshotView{}, fmt.Errorf("short cgroups snapshot response payload")
	}

	view := CgroupsSnapshotView{
		LayoutVersion:  binary.LittleEndian.Uint16(payload[offCgroupsSnapshotRespLayoutVersion : offCgroupsSnapshotRespLayoutVersion+2]),
		Flags:          binary.LittleEndian.Uint16(payload[offCgroupsSnapshotRespFlags : offCgroupsSnapshotRespFlags+2]),
		SystemdEnabled: binary.LittleEndian.Uint32(payload[offCgroupsSnapshotRespSystemd:offCgroupsSnapshotRespSystemd+4]) != 0,
		Generation:     binary.LittleEndian.Uint64(payload[offCgroupsSnapshotRespGeneration : offCgroupsSnapshotRespGeneration+8]),
		ItemCount:      itemCount,
		itemRefs:       payload[CgroupsSnapshotResponseHeaderLen:packedOffset],
		items:          payload[packedOffset:],
	}
	if view.LayoutVersion != CgroupsSnapshotLayoutVersion {
		return CgroupsSnapshotView{}, fmt.Errorf("invalid cgroups snapshot response layout version")
	}

	for i := uint32(0); i < itemCount; i++ {
		if _, err := view.ItemViewAt(i); err != nil {
			return CgroupsSnapshotView{}, err
		}
	}
	return view, nil
}

func (v CgroupsSnapshotView) ItemViewAt(index uint32) (CgroupsSnapshotItemView, error) {
	if index >= v.ItemCount {
		return CgroupsSnapshotItemView{}, fmt.Errorf("cgroups snapshot item index out of bounds")
	}

	base := int(index) * MessageItemRefLen
	offset := binary.LittleEndian.Uint32(v.itemRefs[base+offItemRefOffset : base+offItemRefOffset+4])
	length := binary.LittleEndian.Uint32(v.itemRefs[base+offItemRefLength : base+offItemRefLength+4])
	if int(offset)%MessageItemAlignment != 0 {
		return CgroupsSnapshotItemView{}, fmt.Errorf("cgroups snapshot item offset is not aligned")
	}

	start := int(offset)
	end := start + int(length)
	if end < start || end > len(v.items) || int(length) < CgroupsSnapshotItemHeaderLen {
		return CgroupsSnapshotItemView{}, fmt.Errorf("cgroups snapshot item out of bounds")
	}

	payload := v.items[start:end]
	nameView, err := validateCStringView(
		payload,
		binary.LittleEndian.Uint32(payload[offCgroupsSnapshotItemNameOff:offCgroupsSnapshotItemNameOff+4]),
		binary.LittleEndian.Uint32(payload[offCgroupsSnapshotItemNameLen:offCgroupsSnapshotItemNameLen+4]),
	)
	if err != nil {
		return CgroupsSnapshotItemView{}, err
	}
	pathView, err := validateCStringView(
		payload,
		binary.LittleEndian.Uint32(payload[offCgroupsSnapshotItemPathOff:offCgroupsSnapshotItemPathOff+4]),
		binary.LittleEndian.Uint32(payload[offCgroupsSnapshotItemPathLen:offCgroupsSnapshotItemPathLen+4]),
	)
	if err != nil {
		return CgroupsSnapshotItemView{}, err
	}

	view := CgroupsSnapshotItemView{
		LayoutVersion: binary.LittleEndian.Uint16(payload[offCgroupsSnapshotItemLayoutVersion : offCgroupsSnapshotItemLayoutVersion+2]),
		Flags:         binary.LittleEndian.Uint16(payload[offCgroupsSnapshotItemFlags : offCgroupsSnapshotItemFlags+2]),
		Hash:          binary.LittleEndian.Uint32(payload[offCgroupsSnapshotItemHash : offCgroupsSnapshotItemHash+4]),
		Options:       binary.LittleEndian.Uint32(payload[offCgroupsSnapshotItemOptions : offCgroupsSnapshotItemOptions+4]),
		Enabled:       binary.LittleEndian.Uint32(payload[offCgroupsSnapshotItemEnabled:offCgroupsSnapshotItemEnabled+4]) != 0,
		NameView:      nameView,
		PathView:      pathView,
	}
	if view.LayoutVersion != CgroupsSnapshotLayoutVersion {
		return CgroupsSnapshotItemView{}, fmt.Errorf("invalid cgroups snapshot item layout version")
	}
	return view, nil
}

func NewCgroupsSnapshotResponseBuilder(
	buf []byte,
	generation uint64,
	systemdEnabled bool,
	flags uint16,
	expectedItems uint32,
) (*CgroupsSnapshotResponseBuilder, error) {
	tableLen := int(expectedItems) * MessageItemRefLen
	if expectedItems != 0 && tableLen/MessageItemRefLen != int(expectedItems) {
		return nil, fmt.Errorf("item-ref table size overflow")
	}
	packedOffset := CgroupsSnapshotResponseHeaderLen + tableLen
	if packedOffset < CgroupsSnapshotResponseHeaderLen || len(buf) < packedOffset {
		return nil, fmt.Errorf("snapshot builder buffer is too small")
	}
	for i := 0; i < packedOffset; i++ {
		buf[i] = 0
	}
	return &CgroupsSnapshotResponseBuilder{
		buf:            buf,
		payloadLen:     packedOffset,
		packedOffset:   packedOffset,
		expectedItems:  expectedItems,
		flags:          flags,
		systemdEnabled: systemdEnabled,
		generation:     generation,
	}, nil
}

func (b *CgroupsSnapshotResponseBuilder) AddItem(item CgroupsSnapshotItem) error {
	if b.itemCount >= b.expectedItems {
		return fmt.Errorf("too many cgroups snapshot items")
	}
	itemLen, err := CgroupsSnapshotItemPayloadLen(item)
	if err != nil {
		return err
	}
	start, err := AlignedItemSize(uint32(b.payloadLen))
	if err != nil {
		return err
	}
	end := start + itemLen
	if end < start || end > len(b.buf) {
		return fmt.Errorf("snapshot builder buffer is too small")
	}
	for i := b.payloadLen; i < start; i++ {
		b.buf[i] = 0
	}

	itemBuf := b.buf[start:end]
	for i := range itemBuf {
		itemBuf[i] = 0
	}
	nameOff := uint32(CgroupsSnapshotItemHeaderLen)
	pathOff := nameOff + uint32(len(item.Name)) + 1

	binary.LittleEndian.PutUint16(itemBuf[offCgroupsSnapshotItemLayoutVersion:offCgroupsSnapshotItemLayoutVersion+2], CgroupsSnapshotLayoutVersion)
	binary.LittleEndian.PutUint16(itemBuf[offCgroupsSnapshotItemFlags:offCgroupsSnapshotItemFlags+2], 0)
	binary.LittleEndian.PutUint32(itemBuf[offCgroupsSnapshotItemHash:offCgroupsSnapshotItemHash+4], item.Hash)
	binary.LittleEndian.PutUint32(itemBuf[offCgroupsSnapshotItemOptions:offCgroupsSnapshotItemOptions+4], item.Options)
	if item.Enabled {
		binary.LittleEndian.PutUint32(itemBuf[offCgroupsSnapshotItemEnabled:offCgroupsSnapshotItemEnabled+4], 1)
	}
	binary.LittleEndian.PutUint32(itemBuf[offCgroupsSnapshotItemNameOff:offCgroupsSnapshotItemNameOff+4], nameOff)
	binary.LittleEndian.PutUint32(itemBuf[offCgroupsSnapshotItemNameLen:offCgroupsSnapshotItemNameLen+4], uint32(len(item.Name)))
	binary.LittleEndian.PutUint32(itemBuf[offCgroupsSnapshotItemPathOff:offCgroupsSnapshotItemPathOff+4], pathOff)
	binary.LittleEndian.PutUint32(itemBuf[offCgroupsSnapshotItemPathLen:offCgroupsSnapshotItemPathLen+4], uint32(len(item.Path)))

	copy(itemBuf[CgroupsSnapshotItemHeaderLen:], item.Name)
	itemBuf[CgroupsSnapshotItemHeaderLen+len(item.Name)] = 0
	pathStart := int(pathOff)
	copy(itemBuf[pathStart:], item.Path)
	itemBuf[pathStart+len(item.Path)] = 0

	entryBase := CgroupsSnapshotResponseHeaderLen + int(b.itemCount)*MessageItemRefLen
	binary.LittleEndian.PutUint32(b.buf[entryBase+offItemRefOffset:entryBase+offItemRefOffset+4], uint32(start-b.packedOffset))
	binary.LittleEndian.PutUint32(b.buf[entryBase+offItemRefLength:entryBase+offItemRefLength+4], uint32(itemLen))
	b.itemCount++
	b.payloadLen = end
	return nil
}

func (b *CgroupsSnapshotResponseBuilder) Finish() (int, error) {
	if b.itemCount != b.expectedItems {
		return 0, fmt.Errorf("snapshot builder finished with missing items")
	}
	binary.LittleEndian.PutUint16(b.buf[offCgroupsSnapshotRespLayoutVersion:offCgroupsSnapshotRespLayoutVersion+2], CgroupsSnapshotLayoutVersion)
	binary.LittleEndian.PutUint16(b.buf[offCgroupsSnapshotRespFlags:offCgroupsSnapshotRespFlags+2], b.flags)
	if b.systemdEnabled {
		binary.LittleEndian.PutUint32(b.buf[offCgroupsSnapshotRespSystemd:offCgroupsSnapshotRespSystemd+4], 1)
	}
	binary.LittleEndian.PutUint64(b.buf[offCgroupsSnapshotRespGeneration:offCgroupsSnapshotRespGeneration+8], b.generation)
	return b.payloadLen, nil
}

func encodeBase(kind uint16, messageID uint64) Frame {
	var frame Frame
	header, _ := EncodeMessageHeader(MessageHeader{
		Magic:           MessageMagic,
		Version:         MessageVersion,
		HeaderLen:       MessageHeaderLen,
		Kind:            kind,
		Flags:           0,
		Code:            MethodIncrement,
		TransportStatus: TransportStatusOK,
		PayloadLen:      IncrementPayloadLen,
		ItemCount:       1,
		MessageID:       messageID,
	})
	copy(frame[:MessageHeaderLen], header[:])
	return frame
}

func validateLegacyIncrementHeader(frame Frame, expectedKind uint16) error {
	header, err := DecodeMessageHeader(frame[:])
	if err != nil {
		return err
	}
	if header.Kind != expectedKind ||
		header.Code != MethodIncrement ||
		header.PayloadLen != IncrementPayloadLen ||
		header.ItemCount != 1 ||
		header.Flags != 0 ||
		header.TransportStatus != TransportStatusOK {
		return fmt.Errorf("invalid frame kind/method/payload")
	}
	return nil
}

func EncodeIncrementRequest(requestID uint64, request IncrementRequest) Frame {
	frame := encodeBase(FrameKindRequest, requestID)
	binary.LittleEndian.PutUint64(frame[offValue:offValue+8], request.Value)
	binary.LittleEndian.PutUint32(frame[offStatus:offStatus+4], 0)
	return frame
}

func DecodeIncrementRequest(frame Frame) (uint64, IncrementRequest, error) {
	if err := validateLegacyIncrementHeader(frame, FrameKindRequest); err != nil {
		return 0, IncrementRequest{}, err
	}

	return binary.LittleEndian.Uint64(frame[offMessageID : offMessageID+8]), IncrementRequest{
		Value: binary.LittleEndian.Uint64(frame[offValue : offValue+8]),
	}, nil
}

func EncodeIncrementResponse(requestID uint64, response IncrementResponse) Frame {
	frame := encodeBase(FrameKindResponse, requestID)
	binary.LittleEndian.PutUint64(frame[offValue:offValue+8], response.Value)
	binary.LittleEndian.PutUint32(frame[offStatus:offStatus+4], uint32(response.Status))
	return frame
}

func DecodeIncrementResponse(frame Frame) (uint64, IncrementResponse, error) {
	if err := validateLegacyIncrementHeader(frame, FrameKindResponse); err != nil {
		return 0, IncrementResponse{}, err
	}

	return binary.LittleEndian.Uint64(frame[offMessageID : offMessageID+8]), IncrementResponse{
		Status: int32(binary.LittleEndian.Uint32(frame[offStatus : offStatus+4])),
		Value:  binary.LittleEndian.Uint64(frame[offValue : offValue+8]),
	}, nil
}
