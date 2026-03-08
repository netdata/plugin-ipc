package protocol

import (
	"encoding/binary"
	"fmt"
)

const (
	FrameMagic   uint32 = 0x4e495043
	FrameVersion uint16 = 1
	FrameSize           = 64

	FrameKindRequest  uint16 = 1
	FrameKindResponse uint16 = 2

	MethodIncrement     uint16 = 1
	IncrementPayloadLen uint32 = 12

	StatusOK            int32 = 0
	StatusBadRequest    int32 = 1
	StatusInternalError int32 = 2
)

const (
	offMagic     = 0
	offVersion   = 4
	offKind      = 6
	offMethod    = 8
	offPayload   = 12
	offRequestID = 16
	offValue     = 24
	offStatus    = 32
)

type Frame [FrameSize]byte

type IncrementRequest struct {
	Value uint64
}

type IncrementResponse struct {
	Status int32
	Value  uint64
}

func encodeBase(kind uint16, requestID uint64) Frame {
	var frame Frame
	binary.LittleEndian.PutUint32(frame[offMagic:offMagic+4], FrameMagic)
	binary.LittleEndian.PutUint16(frame[offVersion:offVersion+2], FrameVersion)
	binary.LittleEndian.PutUint16(frame[offKind:offKind+2], kind)
	binary.LittleEndian.PutUint16(frame[offMethod:offMethod+2], MethodIncrement)
	binary.LittleEndian.PutUint32(frame[offPayload:offPayload+4], IncrementPayloadLen)
	binary.LittleEndian.PutUint64(frame[offRequestID:offRequestID+8], requestID)
	return frame
}

func validateHeader(frame Frame, expectedKind uint16) error {
	magic := binary.LittleEndian.Uint32(frame[offMagic : offMagic+4])
	version := binary.LittleEndian.Uint16(frame[offVersion : offVersion+2])
	kind := binary.LittleEndian.Uint16(frame[offKind : offKind+2])
	method := binary.LittleEndian.Uint16(frame[offMethod : offMethod+2])
	payloadLen := binary.LittleEndian.Uint32(frame[offPayload : offPayload+4])

	if magic != FrameMagic || version != FrameVersion {
		return fmt.Errorf("invalid frame magic/version")
	}
	if kind != expectedKind || method != MethodIncrement || payloadLen != IncrementPayloadLen {
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
	if err := validateHeader(frame, FrameKindRequest); err != nil {
		return 0, IncrementRequest{}, err
	}

	return binary.LittleEndian.Uint64(frame[offRequestID : offRequestID+8]), IncrementRequest{
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
	if err := validateHeader(frame, FrameKindResponse); err != nil {
		return 0, IncrementResponse{}, err
	}

	return binary.LittleEndian.Uint64(frame[offRequestID : offRequestID+8]), IncrementResponse{
		Status: int32(binary.LittleEndian.Uint32(frame[offStatus : offStatus+4])),
		Value:  binary.LittleEndian.Uint64(frame[offValue : offValue+8]),
	}, nil
}
