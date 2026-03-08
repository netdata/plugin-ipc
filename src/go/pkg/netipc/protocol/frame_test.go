package protocol

import "testing"

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

func TestRejectsInvalidMagic(t *testing.T) {
	frame := EncodeIncrementRequest(1, IncrementRequest{Value: 1})
	frame[offMagic] = 0
	if _, _, err := DecodeIncrementRequest(frame); err == nil {
		t.Fatal("DecodeIncrementRequest() unexpectedly succeeded")
	}
}
