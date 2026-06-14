//go:build unix

package raw

import "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"

func testLookupClientConfig() posix.ClientConfig {
	return testClientConfig()
}
