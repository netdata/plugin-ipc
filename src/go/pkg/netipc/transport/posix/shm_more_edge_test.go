//go:build linux

package posix

import (
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func defaultShmLayout() (uint32, uint32, uint32, uint32) {
	reqCap := shmAlign64(128)
	respCap := shmAlign64(128)
	reqOff := shmAlign64(uint32(shmHeaderLen))
	respOff := shmAlign64(reqOff + reqCap)
	return reqOff, reqCap, respOff, respCap
}

func fillShmHeader(data []byte, ownerPID int32, ownerGen uint32, reqOff, reqCap, respOff, respCap uint32) {
	if len(data) < int(shmHeaderLen) {
		return
	}
	binary.NativeEndian.PutUint32(data[shmHeaderMagicOff:shmHeaderMagicOff+4], shmRegionMagic)
	binary.NativeEndian.PutUint16(data[shmHeaderVersionOff:shmHeaderVersionOff+2], shmRegionVersion)
	binary.NativeEndian.PutUint16(data[shmHeaderHeaderLenOff:shmHeaderHeaderLenOff+2], uint16(shmHeaderLen))
	binary.NativeEndian.PutUint32(data[shmHeaderOwnerPidOff:shmHeaderOwnerPidOff+4], uint32(ownerPID))
	binary.NativeEndian.PutUint32(data[shmHeaderOwnerGenOff:shmHeaderOwnerGenOff+4], ownerGen)
	binary.NativeEndian.PutUint32(data[shmHeaderReqOffOff:shmHeaderReqOffOff+4], reqOff)
	binary.NativeEndian.PutUint32(data[shmHeaderReqCapOff:shmHeaderReqCapOff+4], reqCap)
	binary.NativeEndian.PutUint32(data[shmHeaderRespOffOff:shmHeaderRespOffOff+4], respOff)
	binary.NativeEndian.PutUint32(data[shmHeaderRespCapOff:shmHeaderRespCapOff+4], respCap)
}

func writeRawShmRegionFile(t *testing.T, runDir, service string, sessionID uint64, size int, fill func([]byte)) string {
	t.Helper()
	if err := os.MkdirAll(runDir, 0700); err != nil {
		t.Fatalf("mkdir %s: %v", runDir, err)
	}

	path, err := buildShmPath(runDir, service, sessionID)
	if err != nil {
		t.Fatalf("build path: %v", err)
	}

	f, err := os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_RDWR, 0600)
	if err != nil {
		t.Fatalf("open %s: %v", path, err)
	}
	defer f.Close()

	if err := f.Truncate(int64(size)); err != nil {
		t.Fatalf("truncate %s: %v", path, err)
	}
	if size == 0 {
		return path
	}

	data := make([]byte, size)
	if fill != nil {
		fill(data)
	}
	if _, err := f.WriteAt(data, 0); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
	return path
}

func TestShmOwnerAliveFalsePaths(t *testing.T) {
	if (&ShmContext{data: make([]byte, 8)}).OwnerAlive() {
		t.Fatal("short header should not be treated as alive")
	}

	runDir := t.TempDir()
	svc := "go_shm_owner_false"
	ctx, err := ShmServerCreate(runDir, svc, 1, 1024, 1024)
	if err != nil {
		t.Fatalf("server create: %v", err)
	}
	defer ctx.ShmDestroy()

	binary.NativeEndian.PutUint32(ctx.data[shmHeaderOwnerPidOff:shmHeaderOwnerPidOff+4], 0)
	if ctx.OwnerAlive() {
		t.Fatal("owner pid 0 should not be treated as alive")
	}

	binary.NativeEndian.PutUint32(ctx.data[shmHeaderOwnerPidOff:shmHeaderOwnerPidOff+4], uint32(int32(os.Getpid())))
	binary.NativeEndian.PutUint32(ctx.data[shmHeaderOwnerGenOff:shmHeaderOwnerGenOff+4], ctx.ownerGeneration+1)
	if ctx.OwnerAlive() {
		t.Fatal("generation mismatch should not be treated as alive")
	}

	ctx.ownerGeneration = 0
	if !ctx.OwnerAlive() {
		t.Fatal("legacy zero cached generation should skip generation check")
	}
}

func TestShmServerCreateRejectsLiveRegion(t *testing.T) {
	runDir := t.TempDir()
	svc := "go_shm_live_region"

	first, err := ShmServerCreate(runDir, svc, 7, 1024, 1024)
	if err != nil {
		t.Fatalf("first create: %v", err)
	}
	defer first.ShmDestroy()

	_, err = ShmServerCreate(runDir, svc, 7, 1024, 1024)
	if !errors.Is(err, ErrShmOpen) {
		t.Fatalf("second create error = %v, want %v", err, ErrShmOpen)
	}
}

func TestShmServerCreateRecoversInvalidAndLegacyFiles(t *testing.T) {
	runDir := t.TempDir()

	t.Run("tiny-invalid-file", func(t *testing.T) {
		svc := "go_shm_recover_tiny"
		writeRawShmRegionFile(t, runDir, svc, 1, 8, nil)

		ctx, err := ShmServerCreate(runDir, svc, 1, 1024, 1024)
		if err != nil {
			t.Fatalf("create after tiny stale file: %v", err)
		}
		defer ctx.ShmDestroy()
	})

	t.Run("zero-generation-legacy-file", func(t *testing.T) {
		svc := "go_shm_recover_legacy"
		reqOff, reqCap, respOff, respCap := defaultShmLayout()
		writeRawShmRegionFile(t, runDir, svc, 2, int(respOff+respCap), func(data []byte) {
			fillShmHeader(data, int32(os.Getpid()), 0, reqOff, reqCap, respOff, respCap)
		})

		ctx, err := ShmServerCreate(runDir, svc, 2, 1024, 1024)
		if err != nil {
			t.Fatalf("create after zero-generation stale file: %v", err)
		}
		defer ctx.ShmDestroy()
	})
}

func TestShmClientAttachRejectsMalformedHeaders(t *testing.T) {
	runDir := t.TempDir()
	reqOff, reqCap, respOff, respCap := defaultShmLayout()
	validSize := int(respOff + respCap)

	tests := []struct {
		name string
		size int
		fill func([]byte)
		want error
	}{
		{
			name: "file-too-small",
			size: 8,
			fill: nil,
			want: ErrShmNotReady,
		},
		{
			name: "bad-magic",
			size: validSize,
			fill: func(data []byte) {
				fillShmHeader(data, int32(os.Getpid()), 1, reqOff, reqCap, respOff, respCap)
				binary.NativeEndian.PutUint32(data[shmHeaderMagicOff:shmHeaderMagicOff+4], 0)
			},
			want: ErrShmBadMagic,
		},
		{
			name: "bad-version",
			size: validSize,
			fill: func(data []byte) {
				fillShmHeader(data, int32(os.Getpid()), 1, reqOff, reqCap, respOff, respCap)
				binary.NativeEndian.PutUint16(data[shmHeaderVersionOff:shmHeaderVersionOff+2], shmRegionVersion+1)
			},
			want: ErrShmBadVersion,
		},
		{
			name: "bad-header-length",
			size: validSize,
			fill: func(data []byte) {
				fillShmHeader(data, int32(os.Getpid()), 1, reqOff, reqCap, respOff, respCap)
				binary.NativeEndian.PutUint16(data[shmHeaderHeaderLenOff:shmHeaderHeaderLenOff+2], 32)
			},
			want: ErrShmBadHeader,
		},
		{
			name: "bad-layout-alignment",
			size: validSize,
			fill: func(data []byte) {
				fillShmHeader(data, int32(os.Getpid()), 1, reqOff+1, reqCap, respOff, respCap)
			},
			want: ErrShmBadSize,
		},
		{
			name: "declared-region-beyond-file-size",
			size: int(shmHeaderLen) + 64,
			fill: func(data []byte) {
				fillShmHeader(data, int32(os.Getpid()), 1, reqOff, reqCap, respOff, respCap)
			},
			want: ErrShmBadSize,
		},
	}

	for i, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			svc := "go_shm_attach_bad"
			writeRawShmRegionFile(t, runDir, svc, uint64(i+1), tc.size, tc.fill)

			_, err := ShmClientAttach(runDir, svc, uint64(i+1))
			if !errors.Is(err, tc.want) {
				t.Fatalf("attach error = %v, want %v", err, tc.want)
			}
		})
	}
}

func TestCheckShmStaleVariants(t *testing.T) {
	runDir := t.TempDir()
	reqOff, reqCap, respOff, respCap := defaultShmLayout()
	validSize := int(respOff + respCap)

	missingPath := filepath.Join(runDir, "missing.ipcshm")
	if got := checkShmStale(missingPath); got != shmStaleNotExist {
		t.Fatalf("missing path result = %v, want %v", got, shmStaleNotExist)
	}

	tinyPath := writeRawShmRegionFile(t, runDir, "go_shm_check_tiny", 1, 8, nil)
	if got := checkShmStale(tinyPath); got != shmStaleInvalid {
		t.Fatalf("tiny file result = %v, want %v", got, shmStaleInvalid)
	}
	if _, err := os.Stat(tinyPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("tiny file should be removed, stat err = %v", err)
	}

	badMagicPath := writeRawShmRegionFile(t, runDir, "go_shm_check_magic", 2, validSize, func(data []byte) {
		fillShmHeader(data, int32(os.Getpid()), 1, reqOff, reqCap, respOff, respCap)
		binary.NativeEndian.PutUint32(data[shmHeaderMagicOff:shmHeaderMagicOff+4], 0)
	})
	if got := checkShmStale(badMagicPath); got != shmStaleInvalid {
		t.Fatalf("bad magic result = %v, want %v", got, shmStaleInvalid)
	}

	livePath := writeRawShmRegionFile(t, runDir, "go_shm_check_live", 3, validSize, func(data []byte) {
		fillShmHeader(data, int32(os.Getpid()), 7, reqOff, reqCap, respOff, respCap)
	})
	if got := checkShmStale(livePath); got != shmStaleLive {
		t.Fatalf("live path result = %v, want %v", got, shmStaleLive)
	}
	if _, err := os.Stat(livePath); err != nil {
		t.Fatalf("live file should remain, stat err = %v", err)
	}

	legacyPath := writeRawShmRegionFile(t, runDir, "go_shm_check_legacy", 4, validSize, func(data []byte) {
		fillShmHeader(data, int32(os.Getpid()), 0, reqOff, reqCap, respOff, respCap)
	})
	if got := checkShmStale(legacyPath); got != shmStaleRecovered {
		t.Fatalf("legacy path result = %v, want %v", got, shmStaleRecovered)
	}
	if _, err := os.Stat(legacyPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("legacy stale file should be removed, stat err = %v", err)
	}
}

func TestShmCleanupStaleMixedEntries(t *testing.T) {
	runDir := t.TempDir()
	reqOff, reqCap, respOff, respCap := defaultShmLayout()
	validSize := int(respOff + respCap)
	svc := "go_shm_cleanup"

	live, err := ShmServerCreate(runDir, svc, 1, 1024, 1024)
	if err != nil {
		t.Fatalf("live server create: %v", err)
	}
	defer live.ShmDestroy()

	tinyPath := writeRawShmRegionFile(t, runDir, svc, 2, 8, nil)
	legacyPath := writeRawShmRegionFile(t, runDir, svc, 3, validSize, func(data []byte) {
		fillShmHeader(data, int32(os.Getpid()), 0, reqOff, reqCap, respOff, respCap)
	})
	unrelatedPath := filepath.Join(runDir, "other-file.ipcshm")
	if err := os.WriteFile(unrelatedPath, []byte("keep"), 0600); err != nil {
		t.Fatalf("write unrelated file: %v", err)
	}
	matchingDir := filepath.Join(runDir, svc+"-dir.ipcshm")
	if err := os.Mkdir(matchingDir, 0700); err != nil {
		t.Fatalf("mkdir matching dir: %v", err)
	}

	ShmCleanupStale(runDir, svc)

	if _, err := os.Stat(live.path); err != nil {
		t.Fatalf("live region should remain, stat err = %v", err)
	}
	if _, err := os.Stat(tinyPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("tiny stale file should be removed, stat err = %v", err)
	}
	if _, err := os.Stat(legacyPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("legacy stale file should be removed, stat err = %v", err)
	}
	if _, err := os.Stat(unrelatedPath); err != nil {
		t.Fatalf("unrelated file should remain, stat err = %v", err)
	}
	if info, err := os.Stat(matchingDir); err != nil || !info.IsDir() {
		t.Fatalf("matching directory should remain, stat err = %v", err)
	}
}
