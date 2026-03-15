//go:build windows

package windows

import "runtime"

// spinPause yields briefly during spin loops. On Windows, Gosched is
// the closest portable equivalent to a CPU pause hint in pure Go.
func spinPause() {
	runtime.Gosched()
}
