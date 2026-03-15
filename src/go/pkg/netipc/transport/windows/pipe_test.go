//go:build windows

package windows

import (
	"testing"
)

func TestFNV1a64Empty(t *testing.T) {
	got := FNV1a64([]byte{})
	if got != fnv1aOffsetBasis {
		t.Errorf("FNV1a64(empty) = %x, want %x", got, fnv1aOffsetBasis)
	}
}

func TestFNV1a64Deterministic(t *testing.T) {
	data := []byte("/var/run/netdata")
	h1 := FNV1a64(data)
	h2 := FNV1a64(data)
	if h1 != h2 {
		t.Errorf("FNV1a64 not deterministic: %x != %x", h1, h2)
	}
}

func TestFNV1a64DifferentInputs(t *testing.T) {
	h1 := FNV1a64([]byte("/var/run/netdata"))
	h2 := FNV1a64([]byte("/tmp/netdata"))
	if h1 == h2 {
		t.Error("FNV1a64 should produce different hashes for different inputs")
	}
}

func TestValidateServiceName(t *testing.T) {
	valid := []string{
		"cgroups-snapshot",
		"test_service.v1",
		"A-Z_09",
		"a",
		"abc123",
	}
	for _, name := range valid {
		if err := validateServiceName(name); err != nil {
			t.Errorf("validateServiceName(%q) = %v, want nil", name, err)
		}
	}

	invalid := []string{
		"",
		".",
		"..",
		"has space",
		"has/slash",
		"has\\backslash",
		"has@at",
		"has:colon",
	}
	for _, name := range invalid {
		if err := validateServiceName(name); err == nil {
			t.Errorf("validateServiceName(%q) = nil, want error", name)
		}
	}
}

func TestBuildPipeName(t *testing.T) {
	name, err := BuildPipeName("/var/run/netdata", "cgroups-snapshot")
	if err != nil {
		t.Fatalf("BuildPipeName failed: %v", err)
	}

	// Should end with NUL
	if name[len(name)-1] != 0 {
		t.Error("pipe name should end with NUL")
	}

	// Convert to narrow string for checking
	narrow := make([]byte, len(name)-1)
	for i, c := range name[:len(name)-1] {
		narrow[i] = byte(c)
	}
	s := string(narrow)

	if s[:len(`\\.\pipe\netipc-`)] != `\\.\pipe\netipc-` {
		t.Errorf("unexpected prefix: %s", s)
	}

	// Should end with service name
	suffix := "-cgroups-snapshot"
	if s[len(s)-len(suffix):] != suffix {
		t.Errorf("unexpected suffix in: %s", s)
	}
}

func TestBuildPipeNameDeterministic(t *testing.T) {
	n1, _ := BuildPipeName("/var/run", "svc")
	n2, _ := BuildPipeName("/var/run", "svc")
	if len(n1) != len(n2) {
		t.Fatal("different lengths")
	}
	for i := range n1 {
		if n1[i] != n2[i] {
			t.Fatalf("mismatch at %d", i)
		}
	}
}

func TestBuildPipeNameDifferentRunDir(t *testing.T) {
	n1, _ := BuildPipeName("/var/run/netdata", "svc")
	n2, _ := BuildPipeName("/tmp/netdata", "svc")
	// They should differ (different hash)
	if len(n1) == len(n2) {
		same := true
		for i := range n1 {
			if n1[i] != n2[i] {
				same = false
				break
			}
		}
		if same {
			t.Error("different run_dir should produce different pipe names")
		}
	}
}

func TestBuildPipeNameInvalidService(t *testing.T) {
	_, err := BuildPipeName("/var/run", "")
	if err == nil {
		t.Error("expected error for empty service name")
	}

	_, err = BuildPipeName("/var/run", "bad/name")
	if err == nil {
		t.Error("expected error for service with /")
	}

	_, err = BuildPipeName("/var/run", ".")
	if err == nil {
		t.Error("expected error for '.'")
	}
}
