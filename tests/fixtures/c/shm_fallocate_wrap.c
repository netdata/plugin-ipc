#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Test-only linker wrapper for deterministic SHM allocation failures:
 * Linux fallocate, FreeBSD posix_fallocate. */

#include "netipc/netipc_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

static uint32_t g_fault_site = NIPC_SHM_TEST_FAULT_NONE;
static uint32_t g_fault_error = 0;

void nipc_shm_test_fault_set(nipc_shm_test_fault_site_t site, int error_number)
{
    __atomic_store_n(&g_fault_site, NIPC_SHM_TEST_FAULT_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fault_error, (uint32_t)error_number, __ATOMIC_RELAXED);
    __atomic_store_n(&g_fault_site, (uint32_t)site, __ATOMIC_RELEASE);
}

void nipc_shm_test_fault_clear(void)
{
    __atomic_store_n(&g_fault_site, NIPC_SHM_TEST_FAULT_NONE, __ATOMIC_RELEASE);
}

static bool take_allocate_fault(void)
{
    uint32_t expected = NIPC_SHM_TEST_FAULT_ALLOCATE;
    if (__atomic_compare_exchange_n(&g_fault_site, &expected,
                                    NIPC_SHM_TEST_FAULT_NONE, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        errno = (int)__atomic_load_n(&g_fault_error, __ATOMIC_RELAXED);
        return true;
    }
    return false;
}

#if defined(__FreeBSD__)
int __real_posix_fallocate(int fd, off_t offset, off_t length);

/* posix_fallocate returns the error instead of setting errno. */
int __wrap_posix_fallocate(int fd, off_t offset, off_t length)
{
    if (take_allocate_fault())
        return errno;

    return __real_posix_fallocate(fd, offset, length);
}
#else
int __real_fallocate(int fd, int mode, off_t offset, off_t length);

int __wrap_fallocate(int fd, int mode, off_t offset, off_t length)
{
    if (take_allocate_fault())
        return -1;

    return __real_fallocate(fd, mode, offset, length);
}

#if defined(__GLIBC__)
/* The library builds its SHM code with _FILE_OFFSET_BITS=64, so on 32-bit
 * glibc it references fallocate64 whatever this file's own layout is. */
int __real_fallocate64(int fd, int mode, off64_t offset, off64_t length);

int __wrap_fallocate64(int fd, int mode, off64_t offset, off64_t length)
{
    if (take_allocate_fault())
        return -1;

    return __real_fallocate64(fd, mode, offset, length);
}
#endif
#endif /* __FreeBSD__ */
