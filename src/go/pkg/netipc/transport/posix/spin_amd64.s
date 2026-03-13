//go:build linux && amd64

#include "textflag.h"

TEXT ·spinPause(SB), NOSPLIT, $0-0
	PAUSE
	RET
