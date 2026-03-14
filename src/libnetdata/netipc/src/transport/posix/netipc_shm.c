/*
 * netipc_shm.c - L1 POSIX SHM transport (Linux only).
 *
 * Shared memory data plane with spin+futex synchronization.
 * Uses the same wire envelope as UDS -- higher levels are unaware
 * of the underlying transport.
 */

#include "netipc/netipc_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <linux/futex.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* Round up to 64-byte alignment. */
static inline uint32_t align64(uint32_t v)
{
    return (v + (NIPC_SHM_REGION_ALIGNMENT - 1)) & ~(uint32_t)(NIPC_SHM_REGION_ALIGNMENT - 1);
}

/* Build SHM file path: {run_dir}/{service_name}.ipcshm */
static int build_shm_path(char *dst, size_t dst_len,
                           const char *run_dir, const char *service_name)
{
    int n = snprintf(dst, dst_len, "%s/%s.ipcshm", run_dir, service_name);
    if (n < 0 || (size_t)n >= dst_len)
        return -1;
    return 0;
}

/* Thin wrapper around the futex syscall. */
static int futex_wake(uint32_t *addr, int count)
{
    return (int)syscall(SYS_futex, addr, FUTEX_WAKE, count, NULL, NULL, 0);
}

static int futex_wait(uint32_t *addr, uint32_t expected,
                      const struct timespec *timeout)
{
    return (int)syscall(SYS_futex, addr, FUTEX_WAIT, expected, timeout, NULL, 0);
}

/* CPU pause hint for spin loops. */
static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#else
    /* generic: compiler barrier */
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
}

/* Check if a PID is alive (without sending a signal). */
static bool pid_alive(pid_t pid)
{
    if (pid <= 0)
        return false;
    return kill(pid, 0) == 0 || errno == EPERM;
}

/* Pointer into the mapped region at a byte offset. */
static inline void *region_ptr(const nipc_shm_ctx_t *ctx, uint32_t offset)
{
    return (uint8_t *)ctx->base + offset;
}

/* Pointer to the header (always at offset 0). Used for non-atomic fields. */
static inline nipc_shm_region_header_t *region_hdr(const nipc_shm_ctx_t *ctx)
{
    return (nipc_shm_region_header_t *)ctx->base;
}

/*
 * Byte-offset accessors for atomic fields. These avoid taking the
 * address of a packed struct member, which GCC warns about.
 */
#define SHM_OFF_REQ_SEQ    32
#define SHM_OFF_RESP_SEQ   40
#define SHM_OFF_REQ_LEN    48
#define SHM_OFF_RESP_LEN   52
#define SHM_OFF_REQ_SIGNAL 56
#define SHM_OFF_RESP_SIGNAL 60

static inline uint64_t *shm_seq_ptr(void *base, int offset)
{
    return (uint64_t *)((uint8_t *)base + offset);
}

static inline uint32_t *shm_u32_ptr(void *base, int offset)
{
    return (uint32_t *)((uint8_t *)base + offset);
}

/* ------------------------------------------------------------------ */
/*  Stale region recovery                                              */
/* ------------------------------------------------------------------ */

/*
 * Returns:
 *   0  = stale (unlinked)
 *  +1  = live server
 *  -1  = doesn't exist
 *  -2  = exists but undersized / invalid (treated as stale, unlinked)
 */
static int check_shm_stale(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    /* Must be at least header-sized to inspect. */
    if ((size_t)st.st_size < NIPC_SHM_HEADER_LEN) {
        unlink(path);
        return -2;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        unlink(path);
        return -2;
    }

    void *map = mmap(NULL, NIPC_SHM_HEADER_LEN, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        unlink(path);
        return -2;
    }

    const nipc_shm_region_header_t *hdr = (const nipc_shm_region_header_t *)map;

    /* Validate magic first. */
    if (hdr->magic != NIPC_SHM_REGION_MAGIC) {
        munmap(map, NIPC_SHM_HEADER_LEN);
        unlink(path);
        return -2;
    }

    int32_t owner = hdr->owner_pid;
    uint32_t gen = hdr->owner_generation;
    munmap(map, NIPC_SHM_HEADER_LEN);

    if (pid_alive((pid_t)owner)) {
        return 1; /* live */
    }

    (void)gen; /* generation is cached on attach for runtime checks */

    /* Dead owner -- stale. */
    unlink(path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Server: create                                                     */
/* ------------------------------------------------------------------ */

nipc_shm_error_t nipc_shm_server_create(const char *run_dir,
                                          const char *service_name,
                                          uint32_t req_capacity,
                                          uint32_t resp_capacity,
                                          nipc_shm_ctx_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    if (!run_dir || !service_name || !out)
        return NIPC_SHM_ERR_BAD_PARAM;

    /* Build path */
    char path[256];
    if (build_shm_path(path, sizeof(path), run_dir, service_name) < 0)
        return NIPC_SHM_ERR_PATH_TOO_LONG;

    /* Stale recovery */
    int stale = check_shm_stale(path);
    if (stale == 1)
        return NIPC_SHM_ERR_ADDR_IN_USE;

    /* Round capacities up to alignment. */
    req_capacity  = align64(req_capacity);
    resp_capacity = align64(resp_capacity);

    uint32_t req_off  = align64(NIPC_SHM_HEADER_LEN);
    uint32_t resp_off = align64(req_off + req_capacity);
    size_t region_size = (size_t)resp_off + resp_capacity;

    /* Create the file. */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return NIPC_SHM_ERR_OPEN;

    if (ftruncate(fd, (off_t)region_size) < 0) {
        close(fd);
        unlink(path);
        return NIPC_SHM_ERR_TRUNCATE;
    }

    void *map = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        unlink(path);
        return NIPC_SHM_ERR_MMAP;
    }

    /* Zero the region first to init all atomics to 0. */
    memset(map, 0, region_size);

    /* Write header. */
    nipc_shm_region_header_t *hdr = (nipc_shm_region_header_t *)map;
    hdr->magic             = NIPC_SHM_REGION_MAGIC;
    hdr->version           = NIPC_SHM_REGION_VERSION;
    hdr->header_len        = NIPC_SHM_HEADER_LEN;
    hdr->owner_pid         = (int32_t)getpid();

    /* Use a time-based generation to detect PID reuse across restarts. */
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        hdr->owner_generation = (uint32_t)(ts.tv_sec ^ (ts.tv_nsec >> 10));
    }
    hdr->request_offset    = req_off;
    hdr->request_capacity  = req_capacity;
    hdr->response_offset   = resp_off;
    hdr->response_capacity = resp_capacity;

    /* Ensure header writes are visible before clients read. */
    __atomic_thread_fence(__ATOMIC_RELEASE);

    /* Fill context. */
    out->role              = NIPC_SHM_ROLE_SERVER;
    out->fd                = fd;
    out->base              = map;
    out->region_size       = region_size;
    out->request_offset    = req_off;
    out->request_capacity  = req_capacity;
    out->response_offset   = resp_off;
    out->response_capacity = resp_capacity;
    out->local_req_seq     = 0;
    out->local_resp_seq    = 0;
    out->spin_tries        = NIPC_SHM_DEFAULT_SPIN;
    out->owner_generation  = hdr->owner_generation;
    strncpy(out->path, path, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';

    return NIPC_SHM_OK;
}

/* ------------------------------------------------------------------ */
/*  Server: destroy                                                    */
/* ------------------------------------------------------------------ */

void nipc_shm_destroy(nipc_shm_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->base && ctx->base != MAP_FAILED) {
        munmap(ctx->base, ctx->region_size);
        ctx->base = NULL;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    if (ctx->path[0]) {
        unlink(ctx->path);
        ctx->path[0] = '\0';
    }

    ctx->region_size = 0;
}

/* ------------------------------------------------------------------ */
/*  Client: attach                                                     */
/* ------------------------------------------------------------------ */

nipc_shm_error_t nipc_shm_client_attach(const char *run_dir,
                                          const char *service_name,
                                          nipc_shm_ctx_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    if (!run_dir || !service_name || !out)
        return NIPC_SHM_ERR_BAD_PARAM;

    char path[256];
    if (build_shm_path(path, sizeof(path), run_dir, service_name) < 0)
        return NIPC_SHM_ERR_PATH_TOO_LONG;

    /* Open the file. */
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return NIPC_SHM_ERR_OPEN;

    /* Check file size. */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NIPC_SHM_ERR_OPEN;
    }

    if ((size_t)st.st_size < NIPC_SHM_HEADER_LEN) {
        close(fd);
        return NIPC_SHM_ERR_NOT_READY;
    }

    /* Map the region. */
    size_t file_size = (size_t)st.st_size;
    void *map = mmap(NULL, file_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NIPC_SHM_ERR_MMAP;
    }

    /* Acquire fence so we see the server's header writes. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    const nipc_shm_region_header_t *hdr =
        (const nipc_shm_region_header_t *)map;

    /* Validate header. */
    if (hdr->magic != NIPC_SHM_REGION_MAGIC) {
        munmap(map, file_size);
        close(fd);
        return NIPC_SHM_ERR_BAD_MAGIC;
    }

    if (hdr->version != NIPC_SHM_REGION_VERSION) {
        munmap(map, file_size);
        close(fd);
        return NIPC_SHM_ERR_BAD_VERSION;
    }

    if (hdr->header_len != NIPC_SHM_HEADER_LEN) {
        munmap(map, file_size);
        close(fd);
        return NIPC_SHM_ERR_BAD_HEADER;
    }

    /* Validate region is large enough for the declared areas. */
    size_t needed = 0;
    size_t req_end  = (size_t)hdr->request_offset + hdr->request_capacity;
    size_t resp_end = (size_t)hdr->response_offset + hdr->response_capacity;
    needed = req_end > resp_end ? req_end : resp_end;

    if (file_size < needed) {
        munmap(map, file_size);
        close(fd);
        return NIPC_SHM_ERR_BAD_SIZE;
    }

    /* Read the current sequence numbers so we don't see stale data. */
    uint64_t cur_req_seq  = __atomic_load_n(
        (uint64_t *)((uint8_t *)map + offsetof(nipc_shm_region_header_t, req_seq)),
        __ATOMIC_ACQUIRE);
    uint64_t cur_resp_seq = __atomic_load_n(
        (uint64_t *)((uint8_t *)map + offsetof(nipc_shm_region_header_t, resp_seq)),
        __ATOMIC_ACQUIRE);

    /* Fill context. */
    out->role              = NIPC_SHM_ROLE_CLIENT;
    out->fd                = fd;
    out->base              = map;
    out->region_size       = file_size;
    out->request_offset    = hdr->request_offset;
    out->request_capacity  = hdr->request_capacity;
    out->response_offset   = hdr->response_offset;
    out->response_capacity = hdr->response_capacity;
    out->local_req_seq     = cur_req_seq;
    out->local_resp_seq    = cur_resp_seq;
    out->spin_tries        = NIPC_SHM_DEFAULT_SPIN;
    out->owner_generation  = hdr->owner_generation;
    strncpy(out->path, path, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';

    return NIPC_SHM_OK;
}

/* ------------------------------------------------------------------ */
/*  Client: close (no unlink)                                          */
/* ------------------------------------------------------------------ */

void nipc_shm_close(nipc_shm_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->base && ctx->base != MAP_FAILED) {
        munmap(ctx->base, ctx->region_size);
        ctx->base = NULL;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    ctx->region_size = 0;
}

/* ------------------------------------------------------------------ */
/*  Data plane: send                                                   */
/* ------------------------------------------------------------------ */

nipc_shm_error_t nipc_shm_send(nipc_shm_ctx_t *ctx,
                                 const void *msg, size_t msg_len)
{
    if (!ctx || !ctx->base || !msg || msg_len == 0)
        return NIPC_SHM_ERR_BAD_PARAM;

    /*
     * Client writes to the request area; server writes to the response
     * area. The direction is determined by role.
     */
    uint32_t area_offset;
    uint32_t area_capacity;
    int seq_off, len_off, sig_off;

    if (ctx->role == NIPC_SHM_ROLE_CLIENT) {
        area_offset   = ctx->request_offset;
        area_capacity = ctx->request_capacity;
        seq_off       = SHM_OFF_REQ_SEQ;
        len_off       = SHM_OFF_REQ_LEN;
        sig_off       = SHM_OFF_REQ_SIGNAL;
    } else {
        area_offset   = ctx->response_offset;
        area_capacity = ctx->response_capacity;
        seq_off       = SHM_OFF_RESP_SEQ;
        len_off       = SHM_OFF_RESP_LEN;
        sig_off       = SHM_OFF_RESP_SIGNAL;
    }

    if (msg_len > area_capacity)
        return NIPC_SHM_ERR_MSG_TOO_LARGE;

    uint64_t *seq_ptr    = shm_seq_ptr(ctx->base, seq_off);
    uint32_t *len_ptr    = shm_u32_ptr(ctx->base, len_off);
    uint32_t *signal_ptr = shm_u32_ptr(ctx->base, sig_off);

    /* 1. Write message data into the area. */
    memcpy(region_ptr(ctx, area_offset), msg, msg_len);

    /* 2. Store message length (release). */
    __atomic_store_n(len_ptr, (uint32_t)msg_len, __ATOMIC_RELEASE);

    /* 3. Increment sequence number (release) to publish. */
    __atomic_add_fetch(seq_ptr, 1, __ATOMIC_RELEASE);

    /* 4. Wake the peer via futex. */
    __atomic_add_fetch(signal_ptr, 1, __ATOMIC_RELEASE);
    futex_wake(signal_ptr, 1);

    /* Track locally. */
    if (ctx->role == NIPC_SHM_ROLE_CLIENT)
        ctx->local_req_seq++;
    else
        ctx->local_resp_seq++;

    return NIPC_SHM_OK;
}

/* ------------------------------------------------------------------ */
/*  Data plane: receive                                                */
/* ------------------------------------------------------------------ */

nipc_shm_error_t nipc_shm_receive(nipc_shm_ctx_t *ctx,
                                    const void **msg_out,
                                    size_t *msg_len_out,
                                    uint32_t timeout_ms)
{
    if (!ctx || !ctx->base || !msg_out || !msg_len_out)
        return NIPC_SHM_ERR_BAD_PARAM;

    /*
     * Server reads from the request area; client reads from the
     * response area.
     */
    uint32_t area_offset;
    int seq_off, len_off, sig_off;
    uint64_t expected_seq;

    if (ctx->role == NIPC_SHM_ROLE_SERVER) {
        area_offset  = ctx->request_offset;
        seq_off      = SHM_OFF_REQ_SEQ;
        len_off      = SHM_OFF_REQ_LEN;
        sig_off      = SHM_OFF_REQ_SIGNAL;
        expected_seq = ctx->local_req_seq + 1;
    } else {
        area_offset  = ctx->response_offset;
        seq_off      = SHM_OFF_RESP_SEQ;
        len_off      = SHM_OFF_RESP_LEN;
        sig_off      = SHM_OFF_RESP_SIGNAL;
        expected_seq = ctx->local_resp_seq + 1;
    }

    uint64_t *seq_ptr    = shm_seq_ptr(ctx->base, seq_off);
    uint32_t *len_ptr    = shm_u32_ptr(ctx->base, len_off);
    uint32_t *signal_ptr = shm_u32_ptr(ctx->base, sig_off);

    /* Phase 1: spin. */
    bool observed = false;
    for (uint32_t i = 0; i < ctx->spin_tries; i++) {
        uint64_t cur = __atomic_load_n(seq_ptr, __ATOMIC_ACQUIRE);
        if (cur >= expected_seq) {
            observed = true;
            break;
        }
        cpu_relax();
    }

    /* Phase 2: futex wait (if spinning didn't observe the advance). */
    if (!observed) {
        /*
         * Read the current signal value before waiting. If the
         * publisher already incremented it, the FUTEX_WAIT will
         * return immediately (expected != actual).
         */
        uint32_t sig_val = __atomic_load_n(signal_ptr, __ATOMIC_ACQUIRE);

        /* Check one more time after reading the signal. */
        uint64_t cur = __atomic_load_n(seq_ptr, __ATOMIC_ACQUIRE);
        if (cur < expected_seq) {
            struct timespec ts;
            struct timespec *tsp = NULL;
            if (timeout_ms > 0) {
                ts.tv_sec  = timeout_ms / 1000;
                ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
                tsp = &ts;
            }

            int ret = futex_wait(signal_ptr, sig_val, tsp);
            if (ret < 0 && errno == ETIMEDOUT)
                return NIPC_SHM_ERR_TIMEOUT;

            /* After waking, verify the sequence advanced. */
            cur = __atomic_load_n(seq_ptr, __ATOMIC_ACQUIRE);
            if (cur < expected_seq)
                return NIPC_SHM_ERR_TIMEOUT;
        }
    }

    /* Read the message length (acquire). */
    uint32_t mlen = __atomic_load_n(len_ptr, __ATOMIC_ACQUIRE);

    /* Point directly into the SHM region (zero-copy). */
    *msg_out     = region_ptr(ctx, area_offset);
    *msg_len_out = mlen;

    /* Advance local tracking. */
    if (ctx->role == NIPC_SHM_ROLE_SERVER)
        ctx->local_req_seq = expected_seq;
    else
        ctx->local_resp_seq = expected_seq;

    return NIPC_SHM_OK;
}

/* ------------------------------------------------------------------ */
/*  Utility                                                            */
/* ------------------------------------------------------------------ */

bool nipc_shm_owner_alive(const nipc_shm_ctx_t *ctx)
{
    if (!ctx || !ctx->base)
        return false;

    const nipc_shm_region_header_t *hdr =
        (const nipc_shm_region_header_t *)ctx->base;

    if (!pid_alive((pid_t)hdr->owner_pid))
        return false;

    /* PID is alive; verify generation matches to detect PID reuse.
     * If owner_generation is 0, skip the check (legacy region). */
    if (ctx->owner_generation != 0 &&
        hdr->owner_generation != ctx->owner_generation)
        return false;

    return true;
}
