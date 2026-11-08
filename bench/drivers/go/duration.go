package main

import (
	"fmt"
	"os"
	"strconv"
)

// parseDurationArg exits on a duration that is not a positive integer: a
// negative value would wrap the uint64 deadline arithmetic.
func parseDurationArg(arg string) int {
	d, err := strconv.Atoi(arg)
	if err != nil || d <= 0 {
		fmt.Fprintf(os.Stderr, "invalid duration %q: want a positive number of seconds\n", arg)
		os.Exit(1)
	}
	return d
}
