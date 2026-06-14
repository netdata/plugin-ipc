//go:build windows

package raw

import windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"

func testLookupClientConfig() windows.ClientConfig {
	return testWinClientConfig()
}
