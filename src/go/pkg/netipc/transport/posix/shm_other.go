//go:build unix && !linux

package posix

import (
	"errors"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

const platformImplementedProfiles = ProfileUDSSeqpacket

type shmServer struct{}
type shmClient struct{}

func newSHMServer(_ string, _ int, _ int) (*shmServer, error) {
	return nil, errors.New("POSIX SHM is not supported on this platform")
}

func newSHMClient(_ string, _ int, _ int, _ time.Duration) (*shmClient, error) {
	return nil, errors.New("POSIX SHM is not supported on this platform")
}

func (s *shmServer) receiveMessage(_ []byte, _ time.Duration) (int, error) {
	return 0, errors.New("POSIX SHM is not supported on this platform")
}

func (s *shmServer) sendMessage(_ []byte) error {
	return errors.New("POSIX SHM is not supported on this platform")
}

func (s *shmServer) receiveFrame(_ time.Duration) (protocol.Frame, error) {
	return protocol.Frame{}, errors.New("POSIX SHM is not supported on this platform")
}

func (s *shmServer) sendFrame(_ protocol.Frame) error {
	return errors.New("POSIX SHM is not supported on this platform")
}

func (s *shmServer) close() error {
	return nil
}

func (c *shmClient) callMessage(_ []byte, _ []byte, _ time.Duration) (int, error) {
	return 0, errors.New("POSIX SHM is not supported on this platform")
}

func (c *shmClient) callFrame(_ protocol.Frame, _ time.Duration) (protocol.Frame, error) {
	return protocol.Frame{}, errors.New("POSIX SHM is not supported on this platform")
}

func (c *shmClient) close() error {
	return nil
}
