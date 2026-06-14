package raw

import (
	"bytes"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

// CgroupsLookupHandler serves a single CGROUPS_LOOKUP service kind.
type CgroupsLookupHandler func(*protocol.CgroupsLookupRequestView, *protocol.CgroupsLookupBuilder) bool

func cgroupsLookupRequestSize(paths [][]byte) (int, error) {
	dirSize, err := checkedLookupMul(len(paths), protocol.LookupDirEntrySize)
	if err != nil {
		return 0, err
	}
	size, err := checkedLookupAdd(protocol.CgroupsLookupReqHdr, dirSize)
	if err != nil {
		return 0, err
	}
	data := size
	for _, path := range paths {
		data, err = checkedLookupAlign8(data)
		if err != nil {
			return 0, err
		}
		data, err = checkedLookupAdd(data, len(path))
		if err != nil {
			return 0, err
		}
		data, err = checkedLookupAdd(data, 1)
		if err != nil {
			return 0, err
		}
	}
	return data, nil
}

func cgroupsLookupNextRequest(paths [][]byte, maxPayload uint32) (int, int, error) {
	if len(paths) == 0 {
		size, err := cgroupsLookupRequestSize(nil)
		return 0, size, err
	}
	if maxPayload == 0 {
		maxPayload = protocol.MaxPayloadDefault
	}
	lo, hi := 1, len(paths)
	bestCount := 0
	bestSize := 0
	for lo <= hi {
		mid := lo + (hi-lo)/2
		size, err := cgroupsLookupRequestSize(paths[:mid])
		if err != nil {
			return 0, 0, err
		}
		if uint64(size) <= uint64(maxPayload) {
			bestCount = mid
			bestSize = size
			lo = mid + 1
			continue
		}
		hi = mid - 1
	}
	if bestCount == 0 {
		return 0, 0, protocol.ErrOverflow
	}
	return bestCount, bestSize, nil
}

func cgroupsLookupOversizedRequestItem(path []byte) ([]byte, error) {
	size, err := checkedLookupAdd(protocol.CgroupsLookupRespHdr, protocol.LookupDirEntrySize)
	if err != nil {
		return nil, err
	}
	size, err = checkedLookupAlign8(size)
	if err != nil {
		return nil, err
	}
	itemSize, err := checkedLookupAdd(protocol.CgroupsLookupItemHdr, len(path))
	if err != nil {
		return nil, err
	}
	itemSize, err = checkedLookupAdd(itemSize, 2)
	if err != nil {
		return nil, err
	}
	size, err = checkedLookupAdd(size, itemSize)
	if err != nil {
		return nil, err
	}

	buf := make([]byte, size)
	builder := protocol.NewCgroupsLookupBuilder(buf, 1, 0)
	if err := builder.Add(protocol.CgroupLookupOversizedItem, 0, path, nil, nil); err != nil {
		return nil, err
	}
	n := builder.Finish()
	view, err := protocol.DecodeCgroupsLookupResponse(buf[:n])
	if err != nil {
		return nil, err
	}
	rawItem, err := view.RawItem(0)
	if err != nil {
		return nil, err
	}
	return cloneLookupRawItem(rawItem), nil
}

// CallCgroupsLookup performs a blocking typed CGROUPS_LOOKUP call.
// The returned view is valid until the next typed call on this client.
func (c *Client) CallCgroupsLookup(paths [][]byte) (*protocol.CgroupsLookupResponseView, error) {
	return c.CallCgroupsLookupWithTimeout(paths, 0)
}

func (c *Client) CallCgroupsLookupWithTimeout(paths [][]byte, timeoutMs uint32) (*protocol.CgroupsLookupResponseView, error) {
	if err := c.validateMethod(protocol.MethodCgroupsLookup); err != nil {
		return nil, err
	}
	if uint64(len(paths)) > uint64(c.maxLogicalLookupItems) {
		return nil, protocol.ErrOverflow
	}
	if err := c.ensureReadyForLogicalLookup(); err != nil {
		return nil, err
	}

	expectedCount, err := checkedLookupU32(len(paths))
	if err != nil {
		return nil, err
	}
	rawItems := make([][]byte, 0, len(paths))
	start := 0
	var generation uint64
	haveGeneration := false
	subcalls := uint32(0)
	deadline := c.newLookupDeadline(timeoutMs)
	for {
		reqCount, reqSize, err := cgroupsLookupNextRequest(paths[start:], c.sessionMaxRequestPayloadBytes())
		if err != nil {
			if err == protocol.ErrOverflow && start < len(paths) {
				oneItemSize, serr := cgroupsLookupRequestSize(paths[start : start+1])
				if serr == nil {
					if cerr := c.ensureLookupRequestCapacity(oneItemSize); cerr == nil {
						continue
					}
				}
				rawItem, serr := cgroupsLookupOversizedRequestItem(paths[start])
				if serr != nil {
					return nil, serr
				}
				rawItems = append(rawItems, rawItem)
				start++
				continue
			}
			return nil, err
		}
		reqPaths := paths[start : start+reqCount]
		reqBuf := ensureClientScratch(&c.requestBuf, reqSize)
		reqLen, err := protocol.EncodeCgroupsLookupRequest(reqPaths, reqBuf)
		if err != nil {
			return nil, err
		}

		remainingTimeout, terr := c.lookupRemainingTimeout(deadline)
		if terr != nil {
			return nil, terr
		}
		subcalls++
		if subcalls > c.maxLogicalLookupSubcalls {
			return nil, protocol.ErrOverflow
		}
		var payload []byte
		if err := c.callWithRetry(func() error {
			_, callPayload, rerr := c.doRawCallWithTimeout(protocol.MethodCgroupsLookup, reqBuf[:reqLen], remainingTimeout)
			if rerr != nil {
				return rerr
			}
			payload = callPayload
			return nil
		}); err != nil {
			return nil, err
		}
		view, derr := protocol.DecodeCgroupsLookupResponse(payload)
		if derr != nil {
			return nil, derr
		}
		if !haveGeneration {
			generation = view.Generation
			haveGeneration = true
		} else if view.Generation != generation {
			return nil, protocol.ErrBadLayout
		}
		reqCountU32, err := checkedLookupU32(len(reqPaths))
		if err != nil {
			return nil, err
		}
		if view.ItemCount != reqCountU32 {
			return nil, protocol.ErrBadItemCount
		}
		payloadExceededAt := -1
		for i, expected := range reqPaths {
			itemIndex, ierr := checkedLookupU32(i)
			if ierr != nil {
				return nil, ierr
			}
			item, ierr := view.Item(itemIndex)
			if ierr != nil {
				return nil, ierr
			}
			if !bytes.Equal(item.Path.Bytes(), expected) {
				return nil, protocol.ErrBadLayout
			}
			if item.Status == protocol.CgroupLookupPayloadExceeded {
				payloadExceededAt = i
				break
			}
			rawItem, ierr := view.RawItem(itemIndex)
			if ierr != nil {
				return nil, ierr
			}
			rawItems = append(rawItems, cloneLookupRawItem(rawItem))
		}
		if payloadExceededAt < 0 {
			start += reqCount
			if start < len(paths) {
				continue
			}
			if uint32(len(rawItems)) != expectedCount {
				return nil, protocol.ErrBadItemCount
			}
			size, serr := lookupRawResponseSize(protocol.CgroupsLookupRespHdr, rawItems)
			if serr != nil {
				return nil, serr
			}
			if uint64(size) > uint64(c.maxLogicalLookupResponseBytes) {
				return nil, protocol.ErrOverflow
			}
			stitched := make([]byte, size)
			n, eerr := protocol.EncodeCgroupsLookupRawResponse(rawItems, generation, stitched)
			if eerr != nil {
				return nil, eerr
			}
			return protocol.DecodeCgroupsLookupResponse(stitched[:n])
		}
		for i := payloadExceededAt; i < len(reqPaths); i++ {
			itemIndex, ierr := checkedLookupU32(i)
			if ierr != nil {
				return nil, ierr
			}
			item, ierr := view.Item(itemIndex)
			if ierr != nil {
				return nil, ierr
			}
			if !bytes.Equal(item.Path.Bytes(), reqPaths[i]) ||
				item.Status != protocol.CgroupLookupPayloadExceeded {
				return nil, protocol.ErrBadLayout
			}
		}
		if payloadExceededAt == 0 {
			return nil, protocol.ErrOverflow
		}
		start += payloadExceededAt
	}
}

// CgroupsLookupDispatch adapts a typed cgroups lookup handler to the raw dispatch shape.
func CgroupsLookupDispatch(handle CgroupsLookupHandler) DispatchHandler {
	if handle == nil {
		return nil
	}
	return func(request []byte, responseBuf []byte) (int, error) {
		req, err := protocol.DecodeCgroupsLookupRequest(request)
		if err != nil {
			return 0, err
		}
		minRequired, err := lookupMinRequired(protocol.CgroupsLookupRespHdr, req.ItemCount)
		if err != nil {
			return 0, err
		}
		if len(responseBuf) < minRequired {
			return 0, protocol.ErrOverflow
		}
		builder := protocol.NewCgroupsLookupBuilder(responseBuf, req.ItemCount, 0)
		if !handle(req, builder) {
			return 0, errHandlerFailed
		}
		if builder.Error() != nil {
			return 0, builder.Error()
		}
		if builder.ItemCount() != req.ItemCount {
			return 0, protocol.ErrBadItemCount
		}
		return builder.Finish(), nil
	}
}
