package main

import (
	"fmt"
	"os"
	"strconv"
)

// maxDurationSec matches the C drivers' one-year bound.
const maxDurationSec = 86400 * 365

// parseDurationArg exits on a duration outside 1..maxDurationSec: a value
// outside it would wrap the deadline and timer arithmetic.
func parseDurationArg(arg string) int {
	d, err := strconv.Atoi(arg)
	if err != nil || d <= 0 || d > maxDurationSec {
		fmt.Fprintf(os.Stderr, "invalid duration %q: want a positive number of seconds\n", arg)
		os.Exit(1)
	}
	return d
}
