//go:build unix

package posix

import (
	"errors"
	"fmt"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

// Send sends one logical message. The caller fills Kind, Code, Flags,
// ItemCount, MessageID in hdr; this function sets Magic/Version/
// HeaderLen/PayloadLen. If the total message exceeds PacketSize,
// it is chunked transparently.
func (s *Session) Send(hdr *protocol.Header, payload []byte) error {
	if s.fd < 0 {
		return wrapErr(ErrBadParam, "session closed")
	}

	if s.role == RoleClient && hdr.Kind == protocol.KindRequest {
		if s.inflightIDs == nil {
			s.inflightIDs = make(map[uint64]struct{})
		}
		if _, exists := s.inflightIDs[hdr.MessageID]; exists {
			return wrapErr(ErrDuplicateMsgID, fmt.Sprintf("message_id %d", hdr.MessageID))
		}
		s.inflightIDs[hdr.MessageID] = struct{}{}
	}

	hdr.Magic = protocol.MagicMsg
	hdr.Version = protocol.Version
	hdr.HeaderLen = protocol.HeaderLen
	hdr.PayloadLen = uint32(len(payload))

	tracked := s.role == RoleClient && hdr.Kind == protocol.KindRequest
	sendErr := s.sendInner(hdr, payload)
	if sendErr != nil && tracked {
		if errors.Is(sendErr, ErrSend) {
			s.failAllInflight()
		} else {
			delete(s.inflightIDs, hdr.MessageID)
		}
	}

	return sendErr
}

func (s *Session) sendInner(hdr *protocol.Header, payload []byte) error {
	totalMsg := protocol.HeaderSize + len(payload)
	if totalMsg <= int(s.PacketSize) {
		var hdrBuf [protocol.HeaderSize]byte
		hdr.Encode(hdrBuf[:])
		return rawSendIov(s.fd, hdrBuf[:], payload)
	}

	chunkPayloadBudget := int(s.PacketSize) - protocol.HeaderSize
	if chunkPayloadBudget <= 0 {
		return wrapErr(ErrBadParam, "packet_size too small")
	}

	firstChunkPayload := min(len(payload), chunkPayloadBudget)
	remainingAfterFirst := len(payload) - firstChunkPayload
	continuationChunks := uint32(0)
	if remainingAfterFirst > 0 {
		continuationChunks = uint32((remainingAfterFirst + chunkPayloadBudget - 1) / chunkPayloadBudget)
	}
	chunkCount := 1 + continuationChunks

	var hdrBuf [protocol.HeaderSize]byte
	hdr.Encode(hdrBuf[:])
	if err := rawSendIov(s.fd, hdrBuf[:], payload[:firstChunkPayload]); err != nil {
		return err
	}

	offset := firstChunkPayload
	for ci := uint32(1); ci < chunkCount; ci++ {
		remaining := len(payload) - offset
		thisChunk := min(remaining, chunkPayloadBudget)

		chk := protocol.ChunkHeader{
			Magic:           protocol.MagicChunk,
			Version:         protocol.Version,
			Flags:           0,
			MessageID:       hdr.MessageID,
			TotalMessageLen: uint32(totalMsg),
			ChunkIndex:      ci,
			ChunkCount:      chunkCount,
			ChunkPayloadLen: uint32(thisChunk),
		}

		var chkBuf [protocol.HeaderSize]byte
		chk.Encode(chkBuf[:])
		if err := rawSendIov(s.fd, chkBuf[:], payload[offset:offset+thisChunk]); err != nil {
			return err
		}

		offset += thisChunk
	}

	return nil
}
